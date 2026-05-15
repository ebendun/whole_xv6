#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_exit(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_write(void);
extern uint64 sys_close(void);
extern uint64 sys_interpose(void);
extern uint64 sys_pgpte(void);
extern uint64 sys_kpgtbl(void);
extern uint64 sys_sigalarm(void);
extern uint64 sys_sigreturn(void);
extern uint64 sys_bind(void);
extern uint64 sys_unbind(void);
extern uint64 sys_send(void);
extern uint64 sys_recv(void);
extern uint64 sys_cpupin(void);
extern uint64 sys_munmap(void);
extern uint64 sys_linux_openat(void);
extern uint64 sys_linux_writev(void);
extern uint64 sys_linux_mmap(void);
extern uint64 sys_linux_newfstatat(void);
extern uint64 sys_linux_brk(void);
extern uint64 sys_linux_gettid(void);
extern uint64 sys_linux_getppid(void);
extern uint64 sys_linux_set_tid_address(void);
extern uint64 sys_linux_set_robust_list(void);
extern uint64 sys_linux_uname(void);
extern uint64 sys_linux_getrandom(void);
extern uint64 sys_linux_exit_group(void);
extern uint64 sys_linux_execve(void);
extern uint64 sys_linux_clone(void);
extern uint64 sys_linux_wait4(void);
extern uint64 sys_linux_pipe2(void);
extern uint64 sys_linux_dup3(void);
extern uint64 sys_linux_fcntl(void);
extern uint64 sys_linux_sendfile(void);
extern uint64 sys_linux_ppoll(void);
extern uint64 sys_linux_mknodat(void);
extern uint64 sys_linux_mkdirat(void);
extern uint64 sys_linux_unlinkat(void);
extern uint64 sys_linux_linkat(void);

static uint64
sys_zero(void)
{
  return 0;
}

static uint64
sys_minus_one(void)
{
  return -1;
}

struct syscall_entry {
  int num;
  uint64 (*fn)(void);
};

static struct syscall_entry syscall_table[] = {
  {17, sys_minus_one},                 // getcwd
  {SYS_dup, sys_dup},
  {SYS_dup3, sys_linux_dup3},
  {25, sys_linux_fcntl},               // fcntl
  {29, sys_minus_one},                 // ioctl
  {SYS_mknodat, sys_linux_mknodat},
  {SYS_mkdirat, sys_linux_mkdirat},
  {SYS_unlinkat, sys_linux_unlinkat},
  {SYS_linkat, sys_linux_linkat},
  {SYS_chdir, sys_chdir},
  {SYS_openat, sys_linux_openat},
  {SYS_close, sys_close},
  {SYS_pipe2, sys_linux_pipe2},
  {SYS_read, sys_read},
  {SYS_write, sys_write},
  {SYS_writev, sys_linux_writev},
  {SYS_sendfile, sys_linux_sendfile},
  {SYS_ppoll, sys_linux_ppoll},
  {78, sys_minus_one},                 // readlinkat
  {SYS_newfstatat, sys_linux_newfstatat},
  {SYS_fstat, sys_fstat},
  {SYS_exit, sys_exit},
  {SYS_exit_group, sys_linux_exit_group},
  {SYS_set_tid_address, sys_linux_set_tid_address},
  {98, sys_minus_one},                 // futex
  {SYS_set_robust_list, sys_linux_set_robust_list},
  {SYS_nanosleep, sys_pause},
  {SYS_clock_gettime, sys_minus_one},
  {SYS_kill, sys_kill},
  {131, sys_zero},                     // tgkill
  {SYS_rt_sigaction, sys_zero},
  {SYS_rt_sigprocmask, sys_zero},
  {143, sys_zero},                     // setregid
  {144, sys_zero},                     // setgid
  {145, sys_zero},                     // setreuid
  {146, sys_zero},                     // setuid
  {SYS_uname, sys_linux_uname},
  {SYS_getpid, sys_getpid},
  {SYS_getppid, sys_linux_getppid},
  {SYS_getuid, sys_zero},
  {SYS_geteuid, sys_zero},
  {SYS_getgid, sys_zero},
  {SYS_getegid, sys_zero},
  {SYS_gettid, sys_linux_gettid},
  {SYS_brk, sys_linux_brk},
  {SYS_munmap, sys_munmap},
  {SYS_clone, sys_linux_clone},
  {SYS_execve, sys_linux_execve},
  {SYS_mmap, sys_linux_mmap},
  {SYS_mprotect, sys_zero},
  {SYS_wait4, sys_linux_wait4},
  {SYS_prlimit64, sys_minus_one},
  {SYS_getrandom, sys_linux_getrandom},

  //just for memory xv6 lab
  {SYS_sbrk, sys_sbrk},
  {SYS_pause, sys_pause},
  {SYS_uptime, sys_uptime},
  {SYS_interpose, sys_interpose},
  {SYS_pgpte, sys_pgpte},
  {SYS_kpgtbl, sys_kpgtbl},
  {SYS_sigalarm, sys_sigalarm},
  {SYS_sigreturn, sys_sigreturn},
  {SYS_bind, sys_bind},
  {SYS_unbind, sys_unbind},
  {SYS_send, sys_send},
  {SYS_recv, sys_recv},
  {SYS_cpupin, sys_cpupin},
};

static uint64 (*syscall_lookup(int num))(void)
{
  for(int i = 0; i < NELEM(syscall_table); i++){
    if(syscall_table[i].num == num)
      return syscall_table[i].fn;
  }
  return 0;
}

void
syscall(void)
{
  int num;
  struct proc *p = myproc();
  uint64 (*fn)(void);

  num = p->trapframe->a7;
  if((fn = syscall_lookup(num)) != 0) {
    p->trapframe->a0 = fn();
  } else {
    printf("%d %s: unknown linux sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
