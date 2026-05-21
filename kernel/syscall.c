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
extern uint64 sys_linux_close(void);
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
extern uint64 sys_linux_getcwd(void);
extern uint64 sys_linux_openat(void);
extern uint64 sys_linux_fstat(void);
extern uint64 sys_linux_getdents64(void);
extern uint64 sys_linux_writev(void);
extern uint64 sys_linux_readv(void);
extern uint64 sys_linux_lseek(void);
extern uint64 sys_linux_pread64(void);
extern uint64 sys_linux_mmap(void);
extern uint64 sys_linux_mprotect(void);
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
extern uint64 sys_linux_renameat(void);
extern uint64 sys_linux_mount(void);
extern uint64 sys_linux_faccessat(void);
extern uint64 sys_linux_readlinkat(void);
extern uint64 sys_linux_gettimeofday(void);
extern uint64 sys_linux_clock_gettime(void);
extern uint64 sys_linux_times(void);
extern uint64 sys_linux_sched_yield(void);
extern uint64 sys_linux_nanosleep(void);
extern uint64 sys_linux_syslog(void);
extern uint64 sys_linux_sysinfo(void);
extern uint64 sys_linux_ioctl(void);
extern uint64 sys_linux_rt_sigtimedwait(void);
extern uint64 sys_linux_rt_sigaction(void);
extern uint64 sys_linux_rt_sigprocmask(void);
extern uint64 sys_linux_rt_sigreturn(void);
extern uint64 sys_linux_tgkill(void);
extern uint64 sys_linux_tkill(void);
extern uint64 sys_linux_prlimit64(void);
extern uint64 sys_linux_setsid(void);
extern uint64 sys_linux_statfs(void);
extern uint64 sys_linux_futex(void);
extern uint64 sys_linux_utimensat(void);
extern uint64 sys_linux_socket(void);
extern uint64 sys_linux_bind(void);
extern uint64 sys_linux_listen(void);
extern uint64 sys_linux_accept(void);
extern uint64 sys_linux_connect(void);
extern uint64 sys_linux_getsockname(void);
extern uint64 sys_linux_sendto(void);
extern uint64 sys_linux_recvfrom(void);
extern uint64 sys_linux_setsockopt(void);

static uint64
sys_linux_success(void)
{
  return 0;
}

struct syscall_entry {
  int num;
  uint64 (*fn)(void);
};

static struct syscall_entry linux_syscalls[] = {
  {SYS_getcwd, sys_linux_getcwd},
  {SYS_dup, sys_dup},
  {SYS_dup3, sys_linux_dup3},
  {SYS_fcntl, sys_linux_fcntl},
  {SYS_ioctl, sys_linux_ioctl},
  {SYS_mknodat, sys_linux_mknodat},
  {SYS_mkdirat, sys_linux_mkdirat},
  {SYS_unlinkat, sys_linux_unlinkat},
  {SYS_linkat, sys_linux_linkat},
  {SYS_renameat, sys_linux_renameat},
  {SYS_umount2, sys_linux_success},
  {SYS_mount, sys_linux_mount},
  {SYS_faccessat, sys_linux_faccessat},
  {SYS_chdir, sys_chdir},
  {SYS_openat, sys_linux_openat},
  {SYS_close, sys_linux_close},
  {SYS_pipe2, sys_linux_pipe2},
  {SYS_getdents64, sys_linux_getdents64},
  {SYS_lseek, sys_linux_lseek},
  {SYS_read, sys_read},
  {SYS_write, sys_write},
  {SYS_readv, sys_linux_readv},
  {SYS_writev, sys_linux_writev},
  {SYS_pread64, sys_linux_pread64},
  {SYS_sendfile, sys_linux_sendfile},
  {SYS_ppoll, sys_linux_ppoll},
  {SYS_readlinkat, sys_linux_readlinkat},
  {SYS_newfstatat, sys_linux_newfstatat},
  {SYS_fstat, sys_linux_fstat},
  {SYS_utimensat, sys_linux_utimensat},
  {SYS_exit, sys_exit},
  {SYS_exit_group, sys_linux_exit_group},
  {SYS_set_tid_address, sys_linux_set_tid_address},
  {SYS_futex, sys_linux_futex},
  {SYS_set_robust_list, sys_linux_set_robust_list},
  {SYS_get_robust_list, sys_linux_success},
  {SYS_nanosleep, sys_linux_nanosleep},
  {SYS_clock_gettime, sys_linux_clock_gettime},
  {SYS_syslog, sys_linux_syslog},
  {SYS_sched_yield, sys_linux_sched_yield},
  {SYS_kill, sys_kill},
  {SYS_rt_sigreturn, sys_linux_rt_sigreturn},
  {SYS_tkill, sys_linux_tkill},
  {SYS_tgkill, sys_linux_tgkill},
  {SYS_rt_sigaction, sys_linux_rt_sigaction},
  {SYS_rt_sigprocmask, sys_linux_rt_sigprocmask},
  {SYS_rt_sigtimedwait, sys_linux_rt_sigtimedwait},
  {SYS_setregid, sys_linux_success},
  {SYS_setgid, sys_linux_success},
  {SYS_setreuid, sys_linux_success},
  {SYS_setuid, sys_linux_success},
  {SYS_times, sys_linux_times},
  {SYS_setsid, sys_linux_setsid},
  {SYS_uname, sys_linux_uname},
  {SYS_gettimeofday, sys_linux_gettimeofday},
  {SYS_getpid, sys_getpid},
  {SYS_getppid, sys_linux_getppid},
  {SYS_getuid, sys_linux_success},
  {SYS_geteuid, sys_linux_success},
  {SYS_getgid, sys_linux_success},
  {SYS_getegid, sys_linux_success},
  {SYS_gettid, sys_linux_gettid},
  {SYS_sysinfo, sys_linux_sysinfo},
  {SYS_socket, sys_linux_socket},
  {SYS_bind_linux, sys_linux_bind},
  {SYS_listen, sys_linux_listen},
  {SYS_accept, sys_linux_accept},
  {SYS_connect, sys_linux_connect},
  {SYS_getsockname, sys_linux_getsockname},
  {SYS_sendto, sys_linux_sendto},
  {SYS_recvfrom, sys_linux_recvfrom},
  {SYS_setsockopt, sys_linux_setsockopt},
  {SYS_brk, sys_linux_brk},
  {SYS_munmap, sys_munmap},
  {SYS_clone, sys_linux_clone},
  {SYS_execve, sys_linux_execve},
  {SYS_mmap, sys_linux_mmap},
  {SYS_mprotect, sys_linux_mprotect},
  {SYS_wait4, sys_linux_wait4},
  {SYS_prlimit64, sys_linux_prlimit64},
  {SYS_statfs, sys_linux_statfs},
  {SYS_renameat2, sys_linux_renameat},
  {SYS_getrandom, sys_linux_getrandom},
};

static struct syscall_entry xv6_syscalls[] = {
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
  for(int i = 0; i < NELEM(linux_syscalls); i++){
    if(linux_syscalls[i].num == num)
      return linux_syscalls[i].fn;
  }
  for(int i = 0; i < NELEM(xv6_syscalls); i++){
    if(xv6_syscalls[i].num == num)
      return xv6_syscalls[i].fn;
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
