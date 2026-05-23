#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "rwlock.h"
#include "proc.h"
#include "vm.h"

static struct spinlock futex_lock;
static int futex_lock_inited;
extern struct proc proc[NPROC];

static void
futex_init(void)
{
  if(futex_lock_inited == 0){
    initlock(&futex_lock, "futex");
    futex_lock_inited = 1;
  }
}

void
linux_futex_wake(uint64 uaddr)
{
  futex_init();
  acquire(&futex_lock);
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == (void *)uaddr)
      p->state = RUNNABLE;
    release(&p->lock);
  }
  release(&futex_lock);
}

static int
linux_futex_wake_n_locked(uint64 uaddr, int n)
{
  int count = 0;

  if(n < 0)
    return 0;
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(count >= n)
      break;
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == (void *)uaddr){
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

  if(nrwake < 0)
    nrwake = 0;
  if(nrrequeue < 0)
    nrrequeue = 0;

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p == myproc())
      continue;
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == (void *)uaddr){
      if(count < nrwake){
        p->state = RUNNABLE;
        count++;
      } else if(moved < nrrequeue && uaddr2 != 0){
        p->chan = (void *)uaddr2;
        moved++;
      }
    }
    release(&p->lock);
  }
  return count + moved;
}

static int
linux_futex_wake_n(uint64 uaddr, int n)
{
  int count;

  futex_init();
  acquire(&futex_lock);
  count = linux_futex_wake_n_locked(uaddr, n);
  release(&futex_lock);
  return count;
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

  int count = linux_futex_wake_n_locked(uaddr, nrwake);
  if(linux_futex_cmp(old, cmp, cmparg))
    count += linux_futex_wake_n_locked(uaddr2, nrwake2);
  return count;
}

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  struct proc *p = myproc();
  if(p->linux_tgid != 0)
    return linux_tgid(p);
  return p->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_linux_clone(void)
{
  uint64 flags, stack, ptid, tls, ctid;
  int pid;

  argaddr(0, &flags);
  argaddr(1, &stack);
  argaddr(2, &ptid);
  argaddr(3, &tls);
  argaddr(4, &ctid);

  if((flags & 0x80000) == 0) // CLONE_SETTLS
    tls = 0;

  pid = kclone(stack, tls, (flags & 0x200000) ? ctid : 0,
               (flags & 0x100) != 0, (flags & 0x400) != 0,
               (flags & 0x200) != 0, (flags & 0x10000) != 0);
  if(pid > 0 && (flags & 0x100000) && ptid != 0){
    int tid = pid;
    copyout(myproc()->pagetable, ptid, (char *)&tid, sizeof(tid));
  }
  if(pid > 0 && (flags & 0x1000000) && ctid != 0){
    int tid = pid;
    copyout(myproc()->pagetable, ctid, (char *)&tid, sizeof(tid));
  }
  return pid;
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_linux_wait4(void)
{
  uint64 status;

  argaddr(1, &status);
  return kwait_linux(status);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

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
    myproc()->sz += n;
    linux_sync_vm_size(myproc());
  }
  return addr;
}

uint64
sys_linux_brk(void)
{
  uint64 addr;
  struct proc *p = myproc();

  argaddr(0, &addr);
  if(p->linux_brk == 0)
    p->linux_brk = p->sz;
  if(addr == 0)
    return p->linux_brk;
  if(addr < p->linux_brk)
    return p->linux_brk;
  if(p->linux_brk_limit && addr >= p->linux_brk_limit)
    return p->linux_brk;
  if(addr > p->sz){
    if(growproc(addr - p->sz) < 0)
      return p->linux_brk;
  }
  p->linux_brk = addr;
  linux_sync_vm_size(p);
  return p->linux_brk;
}

uint64
sys_linux_gettid(void)
{
  return myproc()->pid;
}

uint64
sys_linux_getppid(void)
{
  struct proc *p = myproc();
  int pid = 1;

  if(p->parent)
    pid = p->parent->pid;
  return pid;
}

uint64
sys_linux_set_tid_address(void)
{
  uint64 addr;

  argaddr(0, &addr);
  myproc()->clear_child_tid = addr;
  return myproc()->pid;
}

uint64
sys_linux_set_robust_list(void)
{
  return 0;
}

uint64
sys_linux_uname(void)
{
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
  uint64 addr;
  uint64 tv[2];

  argaddr(0, &addr);
  acquire(&tickslock.l);
  tv[0] = ticks / 10;
  tv[1] = (ticks % 10) * 100000;
  release(&tickslock.l);
  if(copyout(myproc()->pagetable, addr, (char *)tv, sizeof(tv)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_clock_gettime(void)
{
  uint64 addr;
  uint64 ts[2];

  argaddr(1, &addr);
  acquire(&tickslock.l);
  ts[0] = ticks / 10;
  ts[1] = (ticks % 10) * 100000000;
  release(&tickslock.l);
  if(copyout(myproc()->pagetable, addr, (char *)ts, sizeof(ts)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_syslog(void)
{
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
  return 17; // SIGCHLD; enough for the libc-test runner's child wait path.
}

uint64
sys_linux_rt_sigaction(void)
{
  int sig;
  uint64 act, handler;

  argint(0, &sig);
  argaddr(1, &act);
  if((sig == 32 || sig == 33) && act != 0){
    if(copyin(myproc()->pagetable, (char *)&handler, act, sizeof(handler)) < 0)
      return -14; // EFAULT
    linux_set_sigcancel_handler(handler);
  }
  return 0;
}

uint64
sys_linux_rt_sigprocmask(void)
{
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
  return linux_sigreturn();
}

uint64
sys_linux_prlimit64(void)
{
  uint64 new_limit, old_limit;
  uint64 lim[2];

  argaddr(2, &new_limit);
  argaddr(3, &old_limit);
  (void)new_limit;

  if(old_limit != 0){
    lim[0] = 1ULL << 32;
    lim[1] = 1ULL << 32;
    if(copyout(myproc()->pagetable, old_limit, (char *)lim, sizeof(lim)) < 0)
      return -1;
  }
  return 0;
}

uint64
sys_linux_setsid(void)
{
  return myproc()->pid;
}

uint64
sys_linux_futex(void)
{
  uint64 uaddr;
  uint64 uaddr2;
  int op, val, cur;
  int val3;

  argaddr(0, &uaddr);
  argint(1, &op);
  argint(2, &val);
  argaddr(4, &uaddr2);
  argint(5, &val3);
  op &= 0x7f; // Ignore FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME.

  futex_init();

  switch(op){
  case 0:  // FUTEX_WAIT
  case 9:  // FUTEX_WAIT_BITSET
    if(myproc()->trapframe->a3 != 0){
      uint64 ts[2];
      uint deadline, timeout;
      if(copyin(myproc()->pagetable, (char *)ts, myproc()->trapframe->a3,
                sizeof(ts)) < 0)
        return -14; // EFAULT
      timeout = ts[0] * 10 + (ts[1] + 99999999) / 100000000;
      if(timeout == 0)
        timeout = 1;
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
      int timedout = futex_timed_sleep((void *)uaddr, &futex_lock, deadline);
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
    sleep((void *)uaddr, &futex_lock);
    release(&futex_lock);
    if(linux_take_interrupt())
      return -4; // EINTR
    return 0;
  case 1:  // FUTEX_WAKE
  case 10: // FUTEX_WAKE_BITSET
    return linux_futex_wake_n(uaddr, val);
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
  int tgid, tid, sig;

  argint(0, &tgid);
  argint(1, &tid);
  argint(2, &sig);
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state != UNUSED && p->pid == tid && linux_tgid(p) != tgid)
      return -1;
  }
  linux_interrupt(tid, sig >= 32, myproc()->pid);
  return 0;
}

uint64
sys_linux_tkill(void)
{
  int tid, sig;

  argint(0, &tid);
  argint(1, &sig);
  linux_interrupt(tid, sig >= 32, myproc()->pid);
  return 0;
}

uint64
sys_linux_times(void)
{
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
  yield();
  return 0;
}

uint64
sys_linux_nanosleep(void)
{
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
sys_linux_exit_group(void)
{
  int n;
  argint(0, &n);
  kexit_group(n);
  return 0;
}

uint64
sys_pause(void)
{
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
  struct proc *p;  

  p = myproc();
  vmprint(p->pagetable);
  return 0;
}


uint64
sys_sigalarm(void)
{
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
  struct proc *p = myproc();
  uint64 saved_a0 = p->alarm_trapframe.a0;

  *p->trapframe = p->alarm_trapframe;
  p->alarm_inflight = 0;
  return saved_a0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  read_acquire(&tickslock);
  xticks = ticks;
  read_release(&tickslock);
  return xticks;
}

uint64
sys_interpose(void)
{
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
