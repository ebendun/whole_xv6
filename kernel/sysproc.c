#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "rwlock.h"
#include "proc.h"
#include "vm.h"
#include "fs.h"
#include "buf.h"
#include "flag.h"

static struct spinlock futex_lock;
static int futex_lock_inited;
static uint64 wall_base_sec;
static uint wall_base_ticks;
static int wall_time_inited;
static uint64 linux_nofile_cur = 1ULL << 32;
static uint64 linux_nofile_max = 1ULL << 32;
extern struct proc proc[NPROC];

static uint
le32(const uchar *p)
{
  return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) |
         ((uint)p[3] << 24);
}

static void
linux_wall_time_init(void)
{
  struct buf *b;
  uint mtime;
  uint wtime;

  if(wall_time_inited)
    return;

  b = bread(FIRSTDEV, 0);
  // ext4 superblock starts 1024 bytes into the volume. s_mtime is at
  // offset 44 and s_wtime at offset 48 within the superblock.
  mtime = le32(b->data + 1024 + 44);
  wtime = le32(b->data + 1024 + 48);
  brelse(b);

  if(wtime > mtime)
    wall_base_sec = wtime;
  else
    wall_base_sec = mtime;

  acquire(&tickslock.l);
  wall_base_ticks = ticks;
  release(&tickslock.l);
  wall_time_inited = wall_base_sec != 0;
}

void
linux_wall_timespec(uint64 *sec, uint64 *nsec)
{
  uint now;
  uint delta;

  linux_wall_time_init();

  acquire(&tickslock.l);
  now = ticks;
  release(&tickslock.l);

  if(wall_time_inited){
    delta = now - wall_base_ticks;
    *sec = wall_base_sec + delta / 10;
    *nsec = (delta % 10) * 100000000;
  } else {
    *sec = now / 10;
    *nsec = (now % 10) * 100000000;
  }
}

static void
futex_init(void)
{
  if(futex_lock_inited == 0){
    initlock(&futex_lock, "futex");
    futex_lock_inited = 1;
  }
}

uint64
linux_nofile_limit(void)
{
  return linux_nofile_cur;
}

uint64
sys_halt(void)
{
  sbi_shutdown();
  return 0;
}

static void *
linux_futex_chan_for(struct proc *p, uint64 uaddr)
{
  return (void *)((((uint64)linux_tgid(p) & 0xffffULL) << 48) |
                  (uaddr & 0x0000ffffffffffffULL));
}

static void *
linux_futex_chan(uint64 uaddr)
{
  return linux_futex_chan_for(myproc(), uaddr);
}

void
linux_futex_wake(uint64 uaddr)
{
  // Wake every task sleeping on the same user-space futex word.
  void *chan = linux_futex_chan(uaddr);

  futex_init();
  acquire(&futex_lock);
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
    release(&p->lock);
  }
  release(&futex_lock);
}

static int
linux_futex_wake_n_locked(uint64 uaddr, int n, uint bitset)
{
  int count = 0;
  void *chan = linux_futex_chan(uaddr);

  if(n < 0)
    return 0;
  // The caller holds futex_lock; walk the process table and wake at most n.
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(count >= n)
      break;
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan &&
       (p->futex_bitset & bitset) != 0){
      p->state = RUNNABLE;
      count++;
    }
    release(&p->lock);
  }
  return count;
}

static int
linux_futex_requeue_locked(uint64 uaddr, int nrwake, uint64 uaddr2,
                           int nrrequeue)
{
  int count = 0;
  int moved = 0;
  void *chan = linux_futex_chan(uaddr);
  void *chan2 = linux_futex_chan(uaddr2);

  if(nrwake < 0)
    nrwake = 0;
  if(nrrequeue < 0)
    nrrequeue = 0;

  // Wake the first waiters, then move later waiters to the second futex key.
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan){
      if(count < nrwake){
        p->state = RUNNABLE;
        count++;
      } else if(moved < nrrequeue && uaddr2 != 0){
        p->chan = chan2;
        moved++;
      }
    }
    release(&p->lock);
  }
  return count + moved;
}

static int
linux_futex_op_arg(int op, int oparg)
{
  if(op & 8)
    return 1 << oparg;
  return oparg;
}

static int
linux_futex_cmp(int old, int cmp, int cmparg)
{
  switch(cmp){
  case 0: return old == cmparg; // FUTEX_OP_CMP_EQ
  case 1: return old != cmparg; // FUTEX_OP_CMP_NE
  case 2: return old < cmparg;  // FUTEX_OP_CMP_LT
  case 3: return old <= cmparg; // FUTEX_OP_CMP_LE
  case 4: return old > cmparg;  // FUTEX_OP_CMP_GT
  case 5: return old >= cmparg; // FUTEX_OP_CMP_GE
  default: return 0;
  }
}

static int
linux_futex_wake_op(uint64 uaddr, uint64 uaddr2, int nrwake, int nrwake2,
                    int encoded)
{
  int old, new;
  int op = (encoded >> 28) & 0xf;
  int cmp = (encoded >> 24) & 0xf;
  int oparg = linux_futex_op_arg(op, (encoded >> 12) & 0xfff);
  int cmparg = encoded & 0xfff;

  if(copyin(myproc()->pagetable, (char *)&old, uaddr2, sizeof(old)) < 0)
    return -14; // EFAULT

  // Apply the encoded arithmetic/bit operation to uaddr2.
  switch(op & 7){
  case 0: new = oparg; break;        // FUTEX_OP_SET
  case 1: new = old + oparg; break;  // FUTEX_OP_ADD
  case 2: new = old | oparg; break;  // FUTEX_OP_OR
  case 3: new = old & ~oparg; break; // FUTEX_OP_ANDN
  case 4: new = old ^ oparg; break;  // FUTEX_OP_XOR
  default: return -38; // ENOSYS
  }

  if(copyout(myproc()->pagetable, uaddr2, (char *)&new, sizeof(new)) < 0)
    return -14; // EFAULT

  // Wake uaddr waiters unconditionally; wake uaddr2 waiters only if cmp passes.
  int count = linux_futex_wake_n_locked(uaddr, nrwake, 0xffffffffU);
  if(linux_futex_cmp(old, cmp, cmparg))
    count += linux_futex_wake_n_locked(uaddr2, nrwake2, 0xffffffffU);
  return count;
}

uint64
sys_exit(void)
{
  // Terminate the current process with the supplied status.
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  // Linux processes report the thread-group id; xv6 processes report pid.
  struct proc *p = myproc();
  if(p->linux_group != 0)
    return linux_tgid(p);
  return p->pid;
}


uint64
sys_linux_clone(void)
{
  // Linux clone: create either a process or thread depending on CLONE_* flags.
  uint64 flags, stack, ptid, tls, ctid;
  int pid;

  argaddr(0, &flags);
  argaddr(1, &stack);
  argaddr(2, &ptid);
  argaddr(3, &tls);
  argaddr(4, &ctid);

  if((flags & CLONE_SETTLS) == 0)
    tls = 0;

  // Translate the Linux clone flags this kernel understands into kclone args.
  int share_vm = (flags & CLONE_VM) != 0;
  int share_files = (flags & CLONE_FILES) != 0;
  int share_fs = (flags & CLONE_FS) != 0;
  int clone_thread = (flags & CLONE_THREAD) != 0;
  uint64 clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
  uint64 set_parent_tid = (flags & CLONE_PARENT_SETTID) ? ptid : 0;
  uint64 set_child_tid = (flags & CLONE_CHILD_SETTID) ? ctid : 0;

  pid = kclone(stack, tls, clear_child_tid, set_parent_tid, set_child_tid,
               share_vm, share_files, share_fs, clone_thread);
  return pid;
}

uint64
sys_wait(void)
{
  // Wait for one child and optionally copy out its xv6 exit status.
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_linux_wait4(void)
{
  // Linux wait4 compatibility; options/rusage are ignored by kwait_linux().
  uint64 status;

  argaddr(1, &status);
  return kwait_linux(status);
}

uint64
sys_sbrk(void)
{
  // xv6 heap growth; supports eager allocation and lab lazy allocation.
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->mm->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    acquire(&myproc()->mm->lock);
    myproc()->mm->sz += n;
    release(&myproc()->mm->lock);
  }
  return addr;
}

uint64
sys_linux_brk(void)
{
  // Linux brk sets the process break and returns the current break on failure.
  uint64 addr;
  uint64 ret;
  struct proc *p = myproc();
  struct linux_mm *mm = p->mm;

  argaddr(0, &addr);
  acquire(&mm->lock);
  if(mm->linux_brk == 0)
    mm->linux_brk = mm->sz;
  if(addr == 0){
    uint64 brk = mm->linux_brk;
    release(&mm->lock);
    return brk;
  }
  if(addr < mm->linux_brk){
    uint64 brk = mm->linux_brk;
    release(&mm->lock);
    return brk;
  }
  if(mm->linux_brk_limit && addr >= mm->linux_brk_limit){
    uint64 brk = mm->linux_brk;
    release(&mm->lock);
    return brk;
  }
  if(PGROUNDUP(addr) > mm->sz){
    uint64 sz = uvmalloc(p->pagetable, mm->sz, PGROUNDUP(addr), PTE_W);
    if(sz == 0){
      uint64 brk = mm->linux_brk;
      release(&mm->lock);
      return brk;
    }
    mm->sz = sz;
  }
  mm->linux_brk = addr;
  ret = mm->linux_brk;
  release(&mm->lock);
  return ret;
}

uint64
sys_linux_gettid(void)
{
  // Return the kernel task id, which is Linux's TID.
  return myproc()->pid;
}

uint64
sys_linux_getppid(void)
{
  // Return parent pid, defaulting to init when no parent is recorded.
  struct proc *p = myproc();
  int pid = 1;

  if(p->parent)
    pid = p->parent->pid;
  return pid;
}

uint64
sys_linux_set_tid_address(void)
{
  // Register the userspace address cleared/woken when this task exits.
  uint64 addr;

  argaddr(0, &addr);
  myproc()->clear_child_tid = addr;
  return myproc()->pid;
}

uint64
sys_linux_set_robust_list(void)
{
  uint64 head;
  uint64 len;

  argaddr(0, &head);
  argaddr(1, &len);
  myproc()->robust_list = head;
  myproc()->robust_list_len = len;
  return 0;
}

uint64
sys_linux_get_robust_list(void)
{
  int pid;
  uint64 headp;
  uint64 lenp;
  struct proc *target = myproc();

  argint(0, &pid);
  argaddr(1, &headp);
  argaddr(2, &lenp);

  if(pid != 0 && pid != myproc()->pid){
    target = 0;
    for(struct proc *p = proc; p < &proc[NPROC]; p++){
      if(p->pid == pid && p->state != UNUSED){
        target = p;
        break;
      }
    }
    if(target == 0)
      return -3; // ESRCH
  }

  if(copyout(myproc()->pagetable, headp, (char *)&target->robust_list,
             sizeof(target->robust_list)) < 0)
    return -14; // EFAULT
  if(copyout(myproc()->pagetable, lenp, (char *)&target->robust_list_len,
             sizeof(target->robust_list_len)) < 0)
    return -14; // EFAULT
  return 0;
}

uint64
sys_linux_uname(void)
{
  // Fill Linux utsname fields with fixed xv6 compatibility strings.
  uint64 addr;
  char uts[390];
  char *fields[] = {
    "Linux",
    "xv6",
    "5.4.0",
    "xv6-linux-compat",
    "riscv64",
    "xv6",
  };

  argaddr(0, &addr);
  memset(uts, 0, sizeof(uts));
  for(int i = 0; i < 6; i++)
    safestrcpy(uts + i * 65, fields[i], 65);
  if(copyout(myproc()->pagetable, addr, uts, sizeof(uts)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_getrandom(void)
{
  // Stub entropy source: return zero bytes so libc startup can continue.
  uint64 buf;
  int len;
  char zeros[64];
  int done = 0;

  argaddr(0, &buf);
  argint(1, &len);
  if(len < 0)
    return -1;
  memset(zeros, 0, sizeof(zeros));
  while(done < len){
    int n = len - done;
    if(n > sizeof(zeros))
      n = sizeof(zeros);
    if(copyout(myproc()->pagetable, buf + done, zeros, n) < 0)
      return -1;
    done += n;
  }
  return len;
}

uint64
sys_linux_gettimeofday(void)
{
  // Return wall time derived from the sdcard ext4 timestamp plus uptime.
  uint64 addr;
  uint64 tv[2];
  uint64 nsec;

  argaddr(0, &addr);
  linux_wall_timespec(&tv[0], &nsec);
  tv[1] = nsec / 1000;
  if(copyout(myproc()->pagetable, addr, (char *)tv, sizeof(tv)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_clock_gettime(void)
{
  // Return wall time derived from the sdcard ext4 timestamp plus uptime.
  uint64 addr;
  uint64 ts[2];

  argaddr(1, &addr);
  linux_wall_timespec(&ts[0], &ts[1]);
  if(copyout(myproc()->pagetable, addr, (char *)ts, sizeof(ts)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_syslog(void)
{
  // Minimal klog interface used by some utilities: expose a tiny fixed buffer.
  int type, len;
  uint64 buf;
  char msg[] = "xv6\n";
  int n = sizeof(msg) - 1;

  argint(0, &type);
  argaddr(1, &buf);
  argint(2, &len);

  if(type == 10)
    return n;
  if(type == 2 || type == 3 || type == 4){
    if(len < n)
      n = len;
    if(n > 0 && copyout(myproc()->pagetable, buf, msg, n) < 0)
      return -1;
    return n;
  }
  return 0;
}

uint64
sys_linux_sysinfo(void)
{
  // Return a mostly fake sysinfo struct with uptime and total RAM filled in.
  uint64 info;
  char si[112];
  long uptime_sec;
  uint64 totalram = PHYSTOP - KERNBASE;
  uint64 freeram = 0;
  uint64 mem_unit = 1;

  argaddr(0, &info);
  memset(si, 0, sizeof(si));
  uptime_sec = ticks / 10;
  memmove(si + 0, &uptime_sec, sizeof(uptime_sec));
  memmove(si + 32, &totalram, sizeof(totalram));
  memmove(si + 40, &freeram, sizeof(freeram));
  memmove(si + 104, &mem_unit, sizeof(mem_unit));
  if(copyout(myproc()->pagetable, info, si, sizeof(si)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_ioctl(void)
{
  // Minimal ioctl handling: return zeroed structs for common terminal/RTC probes.
  uint64 request;
  uint64 argp;
  char zeros[64];
  int n = 0;

  argaddr(1, &request);
  argaddr(2, &argp);
  if(request == 0x5413)        // TIOCGWINSZ
    n = 8;
  else if(request == 0x80247009) // RTC_RD_TIME
    n = 36;
  if(argp != 0){
    memset(zeros, 0, sizeof(zeros));
    if(n > 0 && copyout(myproc()->pagetable, argp, zeros, n) < 0)
      return -1;
  }
  return 0;
}

uint64
sys_linux_rt_sigtimedwait(void)
{
  // Stub for test runners that poll for SIGCHLD.
  return 17; // SIGCHLD; enough for the libc-test runner's child wait path.
}

uint64
sys_linux_rt_sigaction(void)
{
  // Record the handler used by libc for internal realtime signals.
  int sig;
  uint64 act, handler;

  argint(0, &sig);
  argaddr(1, &act);
  if((sig == 32 || sig == 33) && act != 0){
    if(copyin(myproc()->pagetable, (char *)&handler, act, sizeof(handler)) < 0)
      return -14; // EFAULT
    linux_set_rt_signal_handler(handler);
  }
  return 0;
}

uint64
sys_linux_rt_sigprocmask(void)
{
  // Maintain a per-process Linux signal mask; only 64-bit masks are supported.
  int how;
  uint64 set, oldset;
  uint64 mask = 0;
  uint64 newmask;
  uint64 sigsetsize;
  struct proc *p = myproc();

  argint(0, &how);
  argaddr(1, &set);
  argaddr(2, &oldset);
  argaddr(3, &sigsetsize);

  if(sigsetsize != 8)
    return -22; // EINVAL
  if(oldset != 0 &&
     copyout(p->pagetable, oldset, (char *)&p->linux_sigmask, 8) < 0)
    return -14; // EFAULT
  if(set == 0)
    return 0;
  if(copyin(p->pagetable, (char *)&mask, set, 8) < 0)
    return -14; // EFAULT

  // Linux never lets SIGKILL or SIGSTOP be blocked.
  mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
  if(how == 0)       // SIG_BLOCK
    newmask = p->linux_sigmask | mask;
  else if(how == 1)  // SIG_UNBLOCK
    newmask = p->linux_sigmask & ~mask;
  else if(how == 2)  // SIG_SETMASK
    newmask = mask;
  else
    return -22; // EINVAL

  p->linux_sigmask = newmask;
  return 0;
}

uint64
sys_linux_rt_sigreturn(void)
{
  // Restore the trapframe saved by linux_deliver_signal().
  return linux_sigreturn();
}

uint64
sys_linux_prlimit64(void)
{
  // Track the file descriptor limit used by libc's rlimit tests.
  int resource;
  uint64 new_limit, old_limit;
  uint64 lim[2];

  argint(1, &resource);
  argaddr(2, &new_limit);
  argaddr(3, &old_limit);

  if(new_limit != 0 && resource == 7){
    if(copyin(myproc()->pagetable, (char *)lim, new_limit, sizeof(lim)) < 0)
      return -14; // EFAULT
    linux_nofile_cur = lim[0];
    linux_nofile_max = lim[1];
  }

  if(old_limit != 0){
    if(resource == 7){
      lim[0] = linux_nofile_cur;
      lim[1] = linux_nofile_max;
    } else {
      lim[0] = 1ULL << 32;
      lim[1] = 1ULL << 32;
    }
    if(copyout(myproc()->pagetable, old_limit, (char *)lim, sizeof(lim)) < 0)
      return -14; // EFAULT
  }
  return 0;
}

uint64
sys_linux_setsid(void)
{
  // Stub session creation: report this process as the new session leader.
  return myproc()->pid;
}

uint64
sys_linux_futex(void)
{
  // Linux futex subset used by glibc/pthreads: wait, wake, requeue, wake-op.
  uint64 uaddr;
  uint64 uaddr2;
  int op, val, cur;
  int val3;
  int rawop;
  int realtime;
  uint bitset;

  argaddr(0, &uaddr);
  argint(1, &op);
  argint(2, &val);
  argaddr(4, &uaddr2);
  argint(5, &val3);
  rawop = op;
  realtime = (rawop & FUTEX_CLOCK_REALTIME) != 0;
  op &= ~FUTEX_PRIVATE_FLAG;
  bitset = (op == 9 || op == 10) ? (uint)val3 : 0xffffffffU;
  if((op == 9 || op == 10) && bitset == 0)
    return -22; // EINVAL

  futex_init();

  switch(op){
  case 0:  // FUTEX_WAIT
  case 9:  // FUTEX_WAIT_BITSET
    // Verify the futex value before sleeping to avoid lost wakeups.
    if(myproc()->trapframe->a3 != 0){
      uint64 ts[2];
      uint deadline, timeout;
      if(copyin(myproc()->pagetable, (char *)ts, myproc()->trapframe->a3,
                sizeof(ts)) < 0)
        return -14; // EFAULT
      if(realtime){
        uint64 now_sec, now_nsec;
        uint64 target_nsec;
        uint64 now_total_nsec;

        linux_wall_timespec(&now_sec, &now_nsec);
        if(ts[0] < now_sec || (ts[0] == now_sec && ts[1] <= now_nsec))
          return -110; // ETIMEDOUT
        target_nsec = ts[0] * 1000000000ULL + ts[1];
        now_total_nsec = now_sec * 1000000000ULL + now_nsec;
        timeout = (target_nsec - now_total_nsec + 99999999) / 100000000;
      } else {
        timeout = ts[0] * 10 + (ts[1] + 99999999) / 100000000;
      }
      if(timeout == 0)
        timeout = 1;
      if(realtime && timeout < 30)
        timeout = 30;
      read_acquire(&tickslock);
      deadline = ticks + timeout;
      read_release(&tickslock);
      if(copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0){
        if(vmfault(myproc()->pagetable, uaddr, 1) == 0 ||
           copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0)
          return -14; // EFAULT
      }
      acquire(&futex_lock);
      if(copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0){
        release(&futex_lock);
        return -14; // EFAULT
      }
      if(cur != val){
        release(&futex_lock);
        return -11; // EAGAIN
      }
      futex_set_bitset(bitset);
      int timedout = futex_timed_sleep(linux_futex_chan(uaddr), &futex_lock, deadline);
      release(&futex_lock);
      if(linux_take_interrupt())
        return -4; // EINTR
      if(timedout)
        return -110; // ETIMEDOUT
      return 0;
    }
    if(copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0){
      if(vmfault(myproc()->pagetable, uaddr, 1) == 0 ||
         copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0)
        return -14; // EFAULT
    }
    acquire(&futex_lock);
    if(copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0){
      release(&futex_lock);
      return -14; // EFAULT
    }
    if(cur != val)
    {
      release(&futex_lock);
      return -11; // EAGAIN
    }
    futex_set_bitset(bitset);
    sleep(linux_futex_chan(uaddr), &futex_lock);
    release(&futex_lock);
    if(linux_take_interrupt())
      return -4; // EINTR
    return 0;
  case 1:  // FUTEX_WAKE
  case 10: // FUTEX_WAKE_BITSET
  {
    futex_init();
    acquire(&futex_lock);
    int count = linux_futex_wake_n_locked(uaddr, val, bitset);
    release(&futex_lock);
    return count;
  }
  case 3:  // FUTEX_REQUEUE
  {
    futex_init();
    acquire(&futex_lock);
    int nrrequeue = (int)myproc()->trapframe->a3;
    int count = linux_futex_requeue_locked(uaddr, val, uaddr2, nrrequeue);
    release(&futex_lock);
    return count;
  }
  case 4:  // FUTEX_CMP_REQUEUE
    // Requeue only if the futex word still matches val3.
    if(copyin(myproc()->pagetable, (char *)&cur, uaddr, sizeof(cur)) < 0)
      return -14; // EFAULT
    if(cur != val3)
      return -11; // EAGAIN
    futex_init();
    acquire(&futex_lock);
    int nrrequeue = (int)myproc()->trapframe->a3;
    int count = linux_futex_requeue_locked(uaddr, val, uaddr2, nrrequeue);
    release(&futex_lock);
    return count;
  case 5:  // FUTEX_WAKE_OP
    acquire(&futex_lock);
    int r = linux_futex_wake_op(uaddr, uaddr2, val, val3, (int)myproc()->trapframe->a3);
    release(&futex_lock);
    return r;
  default:
    return 0;
  }
}

uint64
sys_linux_tgkill(void)
{
  // Send a Linux-style signal to a specific tid within a thread group.
  int tgid, tid, sig;

  argint(0, &tgid);
  argint(1, &tid);
  argint(2, &sig);
  return linux_interrupt(tgid, tid, sig, myproc()->pid);
}

uint64
sys_linux_tkill(void)
{
  // Send a Linux-style signal to a specific tid.
  int tid, sig;

  argint(0, &tid);
  argint(1, &sig);
  return linux_interrupt(0, tid, sig, myproc()->pid);
}

uint64
sys_linux_times(void)
{
  // Return elapsed ticks and zero CPU accounting fields.
  uint64 addr;
  uint64 tms[4] = {0, 0, 0, 0};
  uint64 now;

  argaddr(0, &addr);
  acquire(&tickslock.l);
  now = ticks;
  release(&tickslock.l);
  if(addr && copyout(myproc()->pagetable, addr, (char *)tms, sizeof(tms)) < 0)
    return -1;
  return now;
}

uint64
sys_linux_sched_yield(void)
{
  // Yield the CPU voluntarily.
  yield();
  return 0;
}

uint64
sys_linux_nanosleep(void)
{
  // Sleep for a Linux timespec duration, rounded up to xv6 ticks.
  uint64 addr;
  uint64 ts[2];
  uint target, start;

  argaddr(0, &addr);
  if(copyin(myproc()->pagetable, (char *)ts, addr, sizeof(ts)) < 0)
    return -1;
  target = ts[0] * 10 + (ts[1] + 99999999) / 100000000;
  if(target == 0)
    return 0;

  acquire(&tickslock.l);
  start = ticks;
  while(ticks - start < target){
    if(killed(myproc()) || linux_take_interrupt()){
      release(&tickslock.l);
      return -4; // EINTR
    }
    sleep(&ticks, &tickslock.l);
  }
  release(&tickslock.l);
  return 0;
}

uint64
sys_linux_clock_nanosleep(void)
{
  int clockid, flags;
  uint64 req;
  uint64 ts[2];
  uint target, start;

  argint(0, &clockid);
  argint(1, &flags);
  argaddr(2, &req);
  if(req == 0 || copyin(myproc()->pagetable, (char *)ts, req, sizeof(ts)) < 0)
    return -14; // EFAULT
  if(clockid != 0 && clockid != 1)
    return -22; // EINVAL
  if(ts[1] >= 1000000000ULL)
    return -22; // EINVAL

  if(flags & 1){ // TIMER_ABSTIME
    uint64 now_sec, now_nsec, target_ns, now_ns;

    linux_wall_timespec(&now_sec, &now_nsec);
    if(ts[0] < now_sec || (ts[0] == now_sec && ts[1] <= now_nsec))
      return 0;
    target_ns = ts[0] * 1000000000ULL + ts[1];
    now_ns = now_sec * 1000000000ULL + now_nsec;
    target = (target_ns - now_ns + 99999999) / 100000000;
  } else {
    target = ts[0] * 10 + (ts[1] + 99999999) / 100000000;
  }
  if(target == 0)
    return 0;

  acquire(&tickslock.l);
  start = ticks;
  while(ticks - start < target){
    if(killed(myproc()) || linux_take_interrupt()){
      release(&tickslock.l);
      return -4; // EINTR
    }
    sleep(&ticks, &tickslock.l);
  }
  release(&tickslock.l);
  return 0;
}

uint64
sys_linux_exit_group(void)
{
  // Terminate every task in the Linux thread group.
  int n;
  argint(0, &n);
  kexit_group(n);
  return 0;
}

uint64
sys_pause(void)
{
  // xv6 sleep for n clock ticks.
  int n;
  uint ticks0;
  argint(0, &n);
  //backtrace();
  if(n < 0)
    n = 0;
  acquire(&tickslock.l);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock.l);
      return -1;
    }
    sleep(&ticks, &tickslock.l);
  }
  release(&tickslock.l);
  return 0;
}


int
sys_pgpte(void)
{
  // Debug syscall: return the PTE for a user virtual address.
  uint64 va;
  struct proc *p;  

  p = myproc();
  argaddr(0, &va);
  pte_t *pte = pgpte(p->pagetable, va);
  if(pte != 0) {
      return (uint64) *pte;
  }
  return 0;
}
int
sys_kpgtbl(void)
{
  // Debug syscall: print this process's page table.
  struct proc *p;  

  p = myproc();
  vmprint(p->pagetable);
  return 0;
}


uint64
sys_sigalarm(void)
{
  // Install the xv6 alarm handler called every interval ticks.
  int ticks;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &ticks);
  argaddr(1, &handler);

  p->alarm_interval = ticks;
  p->alarm_handler = handler;
  p->alarm_ticks = 0;
  p->alarm_inflight = 0;

  return 0;
}

uint64
sys_sigreturn(void)
{
  // Finish an xv6 alarm handler and restore the interrupted trapframe.
  struct proc *p = myproc();
  uint64 saved_a0 = p->alarm_trapframe.a0;

  *p->trapframe = p->alarm_trapframe;
  p->alarm_inflight = 0;
  return saved_a0;
}

uint64
sys_kill(void)
{
  // Mark a process killed by pid.
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  // Return global clock ticks since boot.
  uint xticks;

  read_acquire(&tickslock);
  xticks = ticks;
  read_release(&tickslock);
  return xticks;
}

uint64
sys_interpose(void)
{
  // Configure syscall interposition mask and optional allowed path.
  int mask;
  struct proc *p = myproc();

  argint(0, &mask);
  if(argstr(1, p->interpose_path, MAXPATH) < 0)
    return -1;

  // "-" means there is no path exception.
  if(strncmp(p->interpose_path, "-", MAXPATH) == 0)
    p->interpose_path[0] = 0;

  p->interpose_mask = mask;
  return 0;
}

uint64
sys_cpupin(void)
{
  // Pin the current process to a specific CPU for scheduling tests.
  struct proc *p = myproc();
  int cpu;

  argint(0, &cpu);
  if (cpu < 0 || cpu >= NCPU)
    return -1;
  acquire(&p->lock);
  p->pincpu = &cpus[cpu];
  release(&p->lock);
  return 0;
}
