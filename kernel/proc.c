#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
static int vmawriteback(struct proc *p, struct vma *v, uint64 addr, uint64 len);

static struct proc *
linux_group_leader(struct proc *p)
{
  if(p == 0)
    return 0;
  if(p->linux_group_leader)
    return p->linux_group_leader;
  return p;
}

int
linux_tgid(struct proc *p)
{
  if(p == 0)
    return -1;
  if(p->linux_tgid)
    return p->linux_tgid;
  if(p->linux_group_leader && p->linux_group_leader->linux_tgid)
    return p->linux_group_leader->linux_tgid;
  return p->pid;
}

static int
linux_same_thread_group(struct proc *a, struct proc *b)
{
  return a && b && linux_tgid(a) == linux_tgid(b);
}

static void
linux_clear_child_tid(struct proc *p)
{
  if(p->clear_child_tid == 0)
    return;

  int zero = 0;
  uint64 pa = walkaddr(p->pagetable, p->clear_child_tid);
  if(pa != 0)
    memmove((void *)pa, &zero, sizeof(zero));

  for(struct proc *q = proc; q < &proc[NPROC]; q++){
    if(q == p || q->state == UNUSED || q->pagetable == 0)
      continue;
    if(!linux_same_thread_group(p, q))
      continue;
    pa = walkaddr(q->pagetable, p->clear_child_tid);
    if(pa != 0)
      memmove((void *)pa, &zero, sizeof(zero));
  }

  linux_futex_wake(p->clear_child_tid);
}

static void
linux_drop_vma_refs(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used && p->vmas[i].f)
      fileclose(p->vmas[i].f);
  }
  memset(p->vmas, 0, sizeof(p->vmas));
}

static int
linux_proc_has_vma(struct proc *p, uint64 a)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used &&
       a >= p->vmas[i].addr &&
       a < p->vmas[i].addr + p->vmas[i].len)
      return 1;
  }
  return 0;
}

static void
linux_preserve_thread_vma_pages(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0)
      continue;
    for(uint64 a = p->vmas[i].addr;
        a < p->vmas[i].addr + PGROUNDUP(p->vmas[i].len);
        a += PGSIZE){
      pte_t *pte = walk(p->pagetable, a, 0);
      if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        continue;

      uint64 pa = PTE2PA(*pte);
      uint flags = PTE_FLAGS(*pte);
      for(struct proc *q = proc; q < &proc[NPROC]; q++){
        if(q == p || q->state == UNUSED || q->state == ZOMBIE ||
           q->pagetable == 0)
          continue;
        if(!linux_same_thread_group(p, q) || !linux_proc_has_vma(q, a))
          continue;
        if(walkaddr(q->pagetable, a) != 0)
          break;
        if(mappages(q->pagetable, a, PGSIZE, pa, flags) == 0)
          kref_inc(pa);
        break;
      }
    }
  }
}

static void
linux_unmap_thread_vmas(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0)
      continue;
    uvmunmap(p->pagetable, p->vmas[i].addr,
             PGROUNDUP(p->vmas[i].len) / PGSIZE, 1);
  }
}

static void
proc_close_files(struct proc *p)
{
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      p->ofile[fd] = 0;
      fileclose(f);
    }
  }
}

static void
proc_put_cwd(struct proc *p)
{
  if(p->cwd == 0)
    return;

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;
}

static void
linux_sync_fd_to_group(struct proc *src, int fd)
{
  if(src == 0 || src->linux_share_files == 0 || fd < 0 || fd >= NOFILE)
    return;

  for(struct proc *q = proc; q < &proc[NPROC]; q++){
    if(q == src || q->state == UNUSED || q->linux_share_files == 0)
      continue;
    if(!linux_same_thread_group(src, q))
      continue;
    if(q->ofile[fd]){
      fileclose(q->ofile[fd]);
      q->ofile[fd] = 0;
    }
    if(src->ofile[fd])
      q->ofile[fd] = filedup(src->ofile[fd]);
  }
}

void
linux_sync_file_table(struct proc *src)
{
  for(int fd = 0; fd < NOFILE; fd++)
    linux_sync_fd_to_group(src, fd);
}

void
linux_sync_vm_size(struct proc *src)
{
  if(src == 0 || src->linux_share_vm == 0)
    return;

  for(struct proc *q = proc; q < &proc[NPROC]; q++){
    if(q == src || q->state == UNUSED)
      continue;
    if(!linux_same_thread_group(src, q))
      continue;
    q->sz = src->sz;
    q->linux_brk = src->linux_brk;
    q->linux_brk_limit = src->linux_brk_limit;
  }
}

static int
copy_mapped_vma_pages(struct proc *src, struct proc *dst)
{
  for(int i = 0; i < NVMA; i++){
    if(src->vmas[i].used == 0)
      continue;
    for(uint64 a = src->vmas[i].addr; a < src->vmas[i].addr + src->vmas[i].len; a += PGSIZE){
      pte_t *pte = walk(src->pagetable, a, 0);
      if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        continue;
      char *mem = kalloc();
      if(mem == 0)
        return -1;
      memmove(mem, (void *)PTE2PA(*pte), PGSIZE);
      if(mappages(dst->pagetable, a, PGSIZE, (uint64)mem, PTE_FLAGS(*pte)) != 0){
        kfree(mem);
        return -1;
      }
    }
  }
  return 0;
}

static int
share_mapped_vma_pages(struct proc *src, struct proc *dst)
{
  for(int i = 0; i < NVMA; i++){
    if(src->vmas[i].used == 0)
      continue;
    for(uint64 a = src->vmas[i].addr; a < src->vmas[i].addr + src->vmas[i].len; a += PGSIZE){
      pte_t *pte = walk(src->pagetable, a, 0);
      if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        continue;
      uint64 pa = PTE2PA(*pte);
      uint flags = PTE_FLAGS(*pte);
      if(mappages(dst->pagetable, a, PGSIZE, pa, flags) != 0)
        return -1;
      kref_inc(pa);
    }
  }
  return 0;
}

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    uint64 va = KSTACK((int) (p - proc));
    for(int i = 0; i < KSTACK_PAGES; i++){
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      kvmmap(kpgtbl, va + i*PGSIZE, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->interpose_mask = 0;
  p->interpose_path[0] = 0;
  p->vfs_root.mount = vfs_root_mount();
  safestrcpy(p->vfs_root.inner, "/", sizeof(p->vfs_root.inner));
  safestrcpy(p->vfs_root.abs_path, "/", sizeof(p->vfs_root.abs_path));
  p->vfs_cwd.mount = vfs_root_mount();
  safestrcpy(p->vfs_cwd.inner, "/", sizeof(p->vfs_cwd.inner));
  safestrcpy(p->vfs_cwd.abs_path, "/", sizeof(p->vfs_cwd.abs_path));
  p->mmap_base = USIGRETURN;
  p->is_linux = 0;
  p->linux_brk = 0;
  p->linux_brk_limit = 0;
  p->linux_signal_pending = 0;
  p->linux_pending_signal = 0;
  p->linux_pending_sender = 0;
  p->linux_in_signal = 0;
  p->linux_share_vm = 0;
  p->linux_share_files = 0;
  p->linux_share_fs = 0;
  p->linux_is_thread = 0;
  p->linux_tgid = p->pid;
  p->linux_group_exiting = 0;
  p->linux_group_xstate = 0;
  p->linux_thread_count = 1;
  p->linux_group_leader = p;
  p->linux_sigcancel_handler = 0;
  p->linux_sigmask = 0;
  p->linux_exe_path[0] = 0;
  memset(p->vmas, 0, sizeof(p->vmas));

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  if((p->usyscall = (struct usyscall *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  memset(p->usyscall, 0, PGSIZE);
  p->usyscall->pid = p->pid;
  p->pincpu = 0;

  if((p->sigreturn = kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  memset(p->sigreturn, 0, PGSIZE);
  ((uint *)p->sigreturn)[0] = 0x08b00893; // addi a7, zero, SYS_rt_sigreturn
  ((uint *)p->sigreturn)[1] = 0x00000073; // ecall

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  memset(&p->alarm_trapframe, 0, sizeof(p->alarm_trapframe));
  p->alarm_interval = 0;
  p->alarm_ticks = 0;
  p->alarm_handler = 0;
  p->alarm_inflight = 0;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + KSTACK_PAGES*PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->usyscall)
    kfree((void*)p->usyscall);
  p->usyscall = 0;
  if(p->sigreturn)
    kfree(p->sigreturn);
  p->sigreturn = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->is_linux = 0;
  p->linux_signal_pending = 0;
  p->linux_pending_signal = 0;
  p->linux_pending_sender = 0;
  p->linux_in_signal = 0;
  p->linux_share_vm = 0;
  p->linux_share_files = 0;
  p->linux_share_fs = 0;
  p->linux_is_thread = 0;
  p->linux_tgid = 0;
  p->linux_group_exiting = 0;
  p->linux_group_xstate = 0;
  p->linux_thread_count = 0;
  p->linux_group_leader = 0;
  p->linux_sigcancel_handler = 0;
  p->linux_sigmask = 0;
  p->linux_brk = 0;
  p->linux_brk_limit = 0;
  p->linux_exe_path[0] = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;

  p->interpose_mask = 0;
  p->interpose_path[0] = 0;
  p->vfs_root.mount = 0;
  p->vfs_root.inner[0] = 0;
  p->vfs_root.abs_path[0] = 0;
  p->vfs_cwd.mount = 0;
  p->vfs_cwd.inner[0] = 0;
  p->vfs_cwd.abs_path[0] = 0;
  
  p->alarm_interval = 0;
  p->alarm_ticks = 0;
  p->alarm_handler = 0;
  p->alarm_inflight = 0;
  p->mmap_base = 0;
  memset(p->vmas, 0, sizeof(p->vmas));
  p->clear_child_tid = 0;
  p->state = UNUSED;
}

void
linux_interrupt(int pid, int deliver_sigcancel, int sender)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED){
      p->linux_signal_pending = 1;
      if(deliver_sigcancel && p->linux_in_signal == 0 &&
         p->linux_sigcancel_handler &&
         p->linux_pending_signal == 0){
        p->linux_pending_signal = 33;
        p->linux_pending_sender = sender;
      }
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&p->lock);
      return;
    }
    release(&p->lock);
  }
}

static struct proc *
linux_thread_leader(struct proc *p)
{
  if(p && p->linux_group_leader)
    return p->linux_group_leader;
  return p;
}

void
linux_set_sigcancel_handler(uint64 handler)
{
  struct proc *cur = myproc();
  struct proc *leader = linux_thread_leader(cur);

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(linux_thread_leader(p) == leader)
      p->linux_sigcancel_handler = handler;
  }
}

int
linux_take_interrupt(void)
{
  struct proc *p = myproc();

  if(p->linux_signal_pending){
    p->linux_signal_pending = 0;
    return 1;
  }
  return 0;
}

#define LINUX_SIGINFO_SIZE 128
#define LINUX_UCONTEXT_OFF LINUX_SIGINFO_SIZE
#define LINUX_UC_MCONTEXT_OFF (LINUX_UCONTEXT_OFF + 176)
#define LINUX_UC_SIZE (176 + 528)

static void
linux_put_regs(char *buf, struct trapframe *tf)
{
  uint64 regs[32];

  regs[0] = tf->epc;
  regs[1] = tf->ra;
  regs[2] = tf->sp;
  regs[3] = tf->gp;
  regs[4] = tf->tp;
  regs[5] = tf->t0;
  regs[6] = tf->t1;
  regs[7] = tf->t2;
  regs[8] = tf->s0;
  regs[9] = tf->s1;
  regs[10] = tf->a0;
  regs[11] = tf->a1;
  regs[12] = tf->a2;
  regs[13] = tf->a3;
  regs[14] = tf->a4;
  regs[15] = tf->a5;
  regs[16] = tf->a6;
  regs[17] = tf->a7;
  regs[18] = tf->s2;
  regs[19] = tf->s3;
  regs[20] = tf->s4;
  regs[21] = tf->s5;
  regs[22] = tf->s6;
  regs[23] = tf->s7;
  regs[24] = tf->s8;
  regs[25] = tf->s9;
  regs[26] = tf->s10;
  regs[27] = tf->s11;
  regs[28] = tf->t3;
  regs[29] = tf->t4;
  regs[30] = tf->t5;
  regs[31] = tf->t6;

  memmove(buf + LINUX_UC_MCONTEXT_OFF, (char *)regs, sizeof(regs));
}

static void
linux_get_regs(char *buf, struct trapframe *tf)
{
  uint64 regs[32];

  memmove((char *)regs, buf + LINUX_UC_MCONTEXT_OFF, sizeof(regs));
  tf->epc = regs[0];
  tf->ra = regs[1];
  tf->sp = regs[2];
  tf->gp = regs[3];
  tf->tp = regs[4];
  tf->t0 = regs[5];
  tf->t1 = regs[6];
  tf->t2 = regs[7];
  tf->s0 = regs[8];
  tf->s1 = regs[9];
  tf->a0 = regs[10];
  tf->a1 = regs[11];
  tf->a2 = regs[12];
  tf->a3 = regs[13];
  tf->a4 = regs[14];
  tf->a5 = regs[15];
  tf->a6 = regs[16];
  tf->a7 = regs[17];
  tf->s2 = regs[18];
  tf->s3 = regs[19];
  tf->s4 = regs[20];
  tf->s5 = regs[21];
  tf->s6 = regs[22];
  tf->s7 = regs[23];
  tf->s8 = regs[24];
  tf->s9 = regs[25];
  tf->s10 = regs[26];
  tf->s11 = regs[27];
  tf->t3 = regs[28];
  tf->t4 = regs[29];
  tf->t5 = regs[30];
  tf->t6 = regs[31];
}

void
linux_deliver_signal(void)
{
  struct proc *p = myproc();
  int sig = p->linux_pending_signal;

  if(sig == 0 || p->linux_sigcancel_handler == 0 || p->linux_in_signal)
    return;

  uint64 frame = (p->trapframe->sp -
                  (LINUX_SIGINFO_SIZE + LINUX_UC_SIZE + 15)) & ~15;
  char *buf = kalloc();
  if(buf == 0){
    setkilled(p);
    return;
  }
  memset(buf, 0, PGSIZE);

  int signo = sig;
  int code = -6; // SI_TKILL
  int sender = p->linux_pending_sender;
  memmove(buf + 0, &signo, sizeof(signo));
  memmove(buf + 8, &code, sizeof(code));
  memmove(buf + 16, &sender, sizeof(sender));

  linux_put_regs(buf, p->trapframe);

  if(copyout(p->pagetable, frame, buf,
             LINUX_SIGINFO_SIZE + LINUX_UC_SIZE) < 0){
    kfree(buf);
    setkilled(p);
    return;
  }
  kfree(buf);

  p->linux_pending_signal = 0;
  p->linux_pending_sender = 0;
  p->linux_in_signal = 1;
  p->trapframe->a0 = sig;
  p->trapframe->a1 = frame;
  p->trapframe->a2 = frame + LINUX_UCONTEXT_OFF;
  p->trapframe->ra = USIGRETURN;
  p->trapframe->sp = frame;
  p->trapframe->epc = p->linux_sigcancel_handler;
}

uint64
linux_sigreturn(void)
{
  struct proc *p = myproc();
  uint64 frame = p->trapframe->sp;
  char *buf = kalloc();

  if(buf == 0)
    return -12; // ENOMEM
  if(copyin(p->pagetable, buf, frame,
            LINUX_SIGINFO_SIZE + LINUX_UC_SIZE) < 0){
    kfree(buf);
    return -14; // EFAULT
  }
  linux_get_regs(buf, p->trapframe);
  kfree(buf);
  p->linux_in_signal = 0;
  return p->trapframe->a0;
}

static int
vmawriteback(struct proc *p, struct vma *v, uint64 addr, uint64 len)
{
  if((v->flags & MAP_SHARED) == 0 || (v->prot & PROT_WRITE) == 0)
    return 0;
  if(v->f == 0)
    return 0;

  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  for(uint64 a = addr; a < addr + len; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    uint64 pageoff = a - v->addr;
    if(pageoff >= v->len || pageoff >= v->filelen)
      continue;
    uint64 nleft = v->filelen - pageoff;
    if(nleft > PGSIZE)
      nleft = PGSIZE;

    char *src = (char*)PTE2PA(*pte);
    uint64 foff = v->offset + pageoff;
    uint64 done = 0;

    while(done < nleft){
      int n1 = nleft - done;
      if(n1 > max)
        n1 = max;

      int r = vfs_file_write_kernel(v->f, src + done, n1, foff + done);
      if(r != n1)
        return -1;
      done += n1;
    }
  }

  return 0;
}

int
proc_munmap(struct proc *p, uint64 addr, uint64 len)
{
  if(len == 0 || (addr % PGSIZE) != 0 || (len % PGSIZE) != 0)
    return -1;

  while(len > 0){
    struct vma *v = 0;
    for(int i = 0; i < NVMA; i++){
      if(p->vmas[i].used == 0)
        continue;
      uint64 start = p->vmas[i].addr;
      uint64 end = start + p->vmas[i].len;
      if(addr >= start && addr < end){
        v = &p->vmas[i];
        break;
      }
    }
    if(v == 0)
      return -1;

    uint64 vend = v->addr + v->len;
    uint64 old_addr = v->addr;
    uint64 old_offset = v->offset;
    uint64 n = len;
    if(addr + n > vend)
      n = vend - addr;
    if(addr != v->addr && addr + n != vend){
      int slot = -1;
      for(int i = 0; i < NVMA; i++){
        if(p->vmas[i].used == 0){
          slot = i;
          break;
        }
      }
      if(slot < 0)
        return -1;
      p->vmas[slot] = *v;
      p->vmas[slot].addr = addr + n;
      p->vmas[slot].offset = old_offset + (addr + n - old_addr);
      p->vmas[slot].len = vend - (addr + n);
      if(p->vmas[slot].filelen > addr + n - old_addr)
        p->vmas[slot].filelen -= addr + n - old_addr;
      else
        p->vmas[slot].filelen = 0;
      if(p->vmas[slot].f)
        filedup(p->vmas[slot].f);
    }

    if(vmawriteback(p, v, addr, n) < 0)
      return -1;

    uvmunmap(p->pagetable, addr, n / PGSIZE, 1);

    if(addr == v->addr && addr + n == vend){
      if(v->f)
        fileclose(v->f);
      memset(v, 0, sizeof(*v));
    } else if(addr == v->addr){
      v->addr += n;
      v->offset += n;
      v->len -= n;
      if(v->filelen > n)
        v->filelen -= n;
      else
        v->filelen = 0;
    } else {
      v->len -= n;
      if(v->filelen > v->len)
        v->filelen = v->len;
    }

    addr += n;
    len -= n;
  }
  return 0;
}

void
proc_munmapall(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0)
      continue;
    proc_munmap(p, p->vmas[i].addr, PGROUNDUP(p->vmas[i].len));
  }
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  if(mappages(pagetable, USYSCALL, PGSIZE,
              (uint64)(p->usyscall), PTE_R | PTE_U) < 0){
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  if(mappages(pagetable, USIGRETURN, PGSIZE,
              (uint64)(p->sigreturn), PTE_R | PTE_X | PTE_U) < 0){
    uvmunmap(pagetable, USYSCALL, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, USYSCALL, 1, 0);
  uvmunmap(pagetable, USIGRETURN, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 oldsz, sz;
  struct proc *p = myproc();

  oldsz = sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  if(p->linux_share_vm){
    for(struct proc *q = proc; q < &proc[NPROC]; q++){
      if(q == p || q->state == UNUSED || !linux_same_thread_group(p, q))
        continue;
      if(n > 0){
        for(uint64 a = PGROUNDUP(oldsz); a < sz; a += PGSIZE){
          pte_t *pte = walk(p->pagetable, a, 0);
          if(pte == 0 || (*pte & PTE_V) == 0)
            continue;
          if(walkaddr(q->pagetable, a) == 0 &&
             mappages(q->pagetable, a, PGSIZE, PTE2PA(*pte), PTE_FLAGS(*pte)) == 0)
            kref_inc(PTE2PA(*pte));
        }
      } else if(n < 0 && PGROUNDUP(sz) < PGROUNDUP(oldsz)){
        uvmunmap(q->pagetable, PGROUNDUP(sz),
                 (PGROUNDUP(oldsz) - PGROUNDUP(sz)) / PGSIZE, 1);
      }
    }
    linux_sync_vm_size(p);
  }
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
static int
forkat(uint64 stack, uint64 tls, uint64 clear_child_tid, int share_vm,
       int share_files, int share_fs, int clone_thread)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  pid = np->pid;
  
  // Copy normal processes with COW. Linux CLONE_VM threads instead get
  // distinct page tables whose user PTEs point at the same physical pages.
  if((share_vm ? uvmshare(p->pagetable, np->pagetable, p->sz)
               : uvmcopy(p->pagetable, np->pagetable, p->sz)) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;


  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;
  if(stack != 0)
    np->trapframe->sp = stack;
  if(tls != 0)
    np->trapframe->tp = tls;
  np->clear_child_tid = clear_child_tid;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  np->vfs_root.mount = p->vfs_root.mount;
  safestrcpy(np->vfs_root.inner, p->vfs_root.inner, sizeof(np->vfs_root.inner));
  safestrcpy(np->vfs_root.abs_path, p->vfs_root.abs_path, sizeof(np->vfs_root.abs_path));
  np->vfs_cwd.mount = p->vfs_cwd.mount;
  safestrcpy(np->vfs_cwd.inner, p->vfs_cwd.inner, sizeof(np->vfs_cwd.inner));
  safestrcpy(np->vfs_cwd.abs_path, p->vfs_cwd.abs_path, sizeof(np->vfs_cwd.abs_path));
  np->interpose_mask = p->interpose_mask;
  safestrcpy(np->interpose_path, p->interpose_path, sizeof(np->interpose_path));
  np->mmap_base = p->mmap_base;
  np->is_linux = p->is_linux;
  np->linux_share_vm = share_vm;
  np->linux_share_files = share_files;
  np->linux_share_fs = share_fs;
  np->linux_sigcancel_handler = p->linux_sigcancel_handler;
  np->linux_sigmask = p->linux_sigmask;
  np->linux_brk = p->linux_brk;
  np->linux_brk_limit = p->linux_brk_limit;
  safestrcpy(np->linux_exe_path, p->linux_exe_path, sizeof(np->linux_exe_path));
  for(i = 0; i < NVMA; i++){
    np->vmas[i] = p->vmas[i];
    if(np->vmas[i].used && np->vmas[i].f)
      filedup(np->vmas[i].f);
  }
  if((share_vm ? share_mapped_vma_pages(p, np)
               : copy_mapped_vma_pages(p, np)) < 0){
    linux_drop_vma_refs(np);
    proc_close_files(np);
    proc_put_cwd(np);
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  np->linux_is_thread = clone_thread;
  if(clone_thread){
    struct proc *leader = linux_group_leader(p);
    np->linux_group_leader = leader;
    np->linux_tgid = linux_tgid(leader);
    np->linux_group_exiting = leader->linux_group_exiting;
    np->linux_group_xstate = leader->linux_group_xstate;
    leader->linux_thread_count++;
  } else {
    np->linux_group_leader = np;
    np->linux_tgid = pid;
    np->linux_group_exiting = 0;
    np->linux_group_xstate = 0;
    np->linux_thread_count = 1;
  }
  if(share_files)
    p->linux_share_files = 1;
  if(share_vm){
    p->linux_share_vm = 1;
    if(p->pincpu == 0)
      p->pincpu = &cpus[0];
    np->pincpu = p->pincpu;
  }
  if(share_fs)
    p->linux_share_fs = 1;

  safestrcpy(np->name, p->name, sizeof(p->name));

  release(&np->lock);
  
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}


int
kclone(uint64 stack, uint64 tls, uint64 clear_child_tid, int share_vm,
       int share_files, int share_fs, int clone_thread)
{
  return forkat(stack, tls, clear_child_tid, share_vm, share_files, share_fs,
                clone_thread);
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();
  struct proc *leader = linux_group_leader(p);
  int thread_only = leader && leader->linux_thread_count > 1;
  int group_dead = 1;

  if(p == initproc)
    panic("init exiting");

  linux_clear_child_tid(p);

  if(!thread_only)
    proc_munmapall(p);
  else {
    linux_preserve_thread_vma_pages(p);
    linux_unmap_thread_vmas(p);
    linux_drop_vma_refs(p);
  }

  // Close all open files.
  proc_close_files(p);

  proc_put_cwd(p);

  acquire(&wait_lock);

  if(leader && leader->linux_thread_count > 0)
    leader->linux_thread_count--;
  if(leader)
    group_dead = leader->linux_thread_count == 0;

  if(group_dead)
    reparent(leader ? leader : p);

  // Parent might be sleeping in wait().
  if(group_dead)
    wakeup(leader ? leader->parent : p->parent);
  
  acquire(&p->lock);

  p->xstate = leader && leader->linux_group_exiting ? leader->linux_group_xstate : status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

void
kexit_group(int status)
{
  struct proc *p = myproc();
  struct proc *leader = linux_group_leader(p);

  acquire(&wait_lock);
  if(leader){
    leader->linux_group_exiting = 1;
    leader->linux_group_xstate = status;
  }
  for(struct proc *q = proc; q < &proc[NPROC]; q++){
    if(q == p || q->state == UNUSED)
      continue;
    if(!linux_same_thread_group(p, q))
      continue;
    acquire(&q->lock);
    q->linux_group_exiting = 1;
    q->linux_group_xstate = status;
    q->killed = 1;
    if(q->state == SLEEPING)
      q->state = RUNNABLE;
    release(&q->lock);
  }
  release(&wait_lock);

  kexit(status);
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        if(linux_same_thread_group(pp, p))
          continue;
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE && pp->linux_thread_count == 0){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int
kwait_options(uint64 addr, int options)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  if((options & 1) == 0)
    return kwait(addr);

  acquire(&wait_lock);
  havekids = 0;
  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      if(linux_same_thread_group(pp, p))
        continue;
      acquire(&pp->lock);
      havekids = 1;
      if(pp->state == ZOMBIE && pp->linux_thread_count == 0){
        pid = pp->pid;
        if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                sizeof(pp->xstate)) < 0){
          release(&pp->lock);
          release(&wait_lock);
          return -1;
        }
        freeproc(pp);
        release(&pp->lock);
        release(&wait_lock);
        return pid;
      }
      release(&pp->lock);
    }
  }
  release(&wait_lock);
  if(!havekids || killed(p))
    return -1;
  return 0;
}

int
kwait_linux(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);
  for(;;){
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        if(linux_same_thread_group(pp, p))
          continue;
        acquire(&pp->lock);
        havekids = 1;
        if(pp->state == ZOMBIE && pp->linux_thread_count == 0){
          int status = pp->xstate << 8;
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&status,
                                  sizeof(status)) < 0){
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    sleep(p, &wait_lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int nproc = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state != UNUSED) {
        nproc++;
      }
      if(p->pincpu && p->pincpu != c) {
        release(&p->lock);
        continue;
      }
      if(p->state == ZOMBIE && linux_group_leader(p) != p){
        freeproc(p);
        release(&p->lock);
        continue;
      }
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
    if(nproc <= 2) {   // only init and sh exist
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(SECONDDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/bin/init", (char *[]){ "/bin/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->futex_bitset = 0xffffffffU;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;
  p->futex_bitset = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

int
futex_timed_sleep(void *chan, struct spinlock *lk, uint deadline)
{
  struct proc *p = myproc();
  int timedout;

  acquire(&p->lock);
  release(lk);

  p->chan = chan;
  p->futex_bitset = 0xffffffffU;
  p->futex_deadline = deadline;
  p->futex_timedout = 0;
  p->state = SLEEPING;

  sched();

  timedout = p->futex_timedout;
  p->chan = 0;
  p->futex_bitset = 0;
  p->futex_deadline = 0;
  p->futex_timedout = 0;

  release(&p->lock);
  acquire(lk);
  return timedout;
}

void
futex_set_bitset(uint bitset)
{
  myproc()->futex_bitset = bitset;
}

void
futex_tick(uint now)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == SLEEPING && p->futex_deadline != 0 &&
       now - p->futex_deadline < 0x80000000U){
      p->futex_timedout = 1;
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;
  int killed_any = 0;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid || linux_tgid(p) == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      killed_any = 1;
      continue;
    }
    release(&p->lock);
  }
  return killed_any ? 0 : -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
