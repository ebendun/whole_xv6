#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#ifdef PGTBL_SOL
#include "riscv.h"
#endif
#include "vm.h"

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
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_linux_clone(void)
{
  uint64 flags, stack;
  int pid;

  argaddr(0, &flags);
  argaddr(1, &stack);
  (void)flags;
  (void)stack;

  pid = kfork();
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
    if(killed(myproc())){
      release(&tickslock.l);
      return -1;
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
  kexit(n);
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
