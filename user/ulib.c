#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/vm.h"
#include "user/user.h"

#define AT_FDCWD -100
#define SIGCHLD 17
#define LINUX_O_CREAT 0x40
#define LINUX_O_TRUNC 0x200
//turn the xv6 syscall to linux syscall
int
fork(void)
{
  return __sys_clone(SIGCHLD, 0, 0, 0, 0);
}

int
exit(int status)
{
  __sys_exit_group(status);
  for(;;)
    ;
}

int
wait(int *status)
{
  return __sys_wait4(-1, status, 0, 0);
}

int
pipe(int *fds)
{
  return __sys_pipe2(fds, 0);
}

int
read(int fd, void *buf, int n)
{
  return __sys_read(fd, buf, n);
}

int
write(int fd, const void *buf, int n)
{
  return __sys_write(fd, buf, n);
}

int
close(int fd)
{
  return __sys_close(fd);
}

int
kill(int pid)
{
  return __sys_kill(pid, 15);
}

int
exec(const char *path, char **argv)
{
  return __sys_execve(path, argv, 0);
}

static int
open_flags_to_linux(int flags)
{
  int oflags = flags & 3;

  if(flags & O_CREATE)
    oflags |= LINUX_O_CREAT;
  if(flags & O_TRUNC)
    oflags |= LINUX_O_TRUNC;
  return oflags;
}

int
open(const char *path, int flags)
{
  return __sys_openat(AT_FDCWD, path, open_flags_to_linux(flags), 0);
}

int
mknod(const char *path, short major, short minor)
{
  return __sys_mknodat(AT_FDCWD, path, major, minor);
}

int
unlink(const char *path)
{
  return __sys_unlinkat(AT_FDCWD, path, 0);
}

int
fstat(int fd, struct stat *st)
{
  return __sys_fstat(fd, st);
}

int
link(const char *old, const char *new)
{
  return __sys_linkat(AT_FDCWD, old, AT_FDCWD, new, 0);
}

int
mkdir(const char *path)
{
  return __sys_mkdirat(AT_FDCWD, path, 0777);
}

int
chdir(const char *path)
{
  return __sys_chdir(path);
}

int
dup(int fd)
{
  return __sys_dup(fd);
}

int
getpid(void)
{
  return __sys_getpid();
}

int
pause(int ticks)
{
  return __sys_pause(ticks);
}

int
uptime(void)
{
  return __sys_uptime();
}

int
interpose(int mask, const char *path)
{
  return __sys_interpose(mask, path);
}

uint64
pgpte(void *va)
{
  return __sys_pgpte(va);
}

void
kpgtbl(void)
{
  __sys_kpgtbl();
}

int
sigalarm(int ticks, void (*handler)())
{
  return __sys_sigalarm(ticks, handler);
}

int
sigreturn(void)
{
  return __sys_sigreturn();
}

void*
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
  return __sys_mmap(addr, len, prot, flags, fd, off);
}

int
munmap(void *addr, size_t len)
{
  return __sys_munmap(addr, len);
}

int
bind(uint16 port)
{
  return __sys_bind(port);
}

int
unbind(uint16 port)
{
  return __sys_unbind(port);
}

int
send(uint16 sport, uint32 dst, uint16 dport, char *buf, uint32 len)
{
  return __sys_send(sport, dst, dport, buf, len);
}

int
recv(uint16 dport, uint32 *src, uint16 *sport, char *buf, uint32 maxlen)
{
  return __sys_recv(dport, src, sport, buf, maxlen);
}

int
cpupin(int cpu)
{
  return __sys_cpupin(cpu);
}

//
// wrapper so that it's OK if main() does not call exit().
//
void
start(int argc, char **argv)
{
  int r;
  extern int main(int argc, char **argv);
  r = main(argc, argv);
  exit(r);
}

char*
strcpy(char *s, const char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

uint
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}

void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

char*
strchr(const char *s, char c)
{
  for(; *s; s++)
    if(*s == c)
      return (char*)s;
  return 0;
}

char*
gets(char *buf, int max)
{
  int i, cc;
  char c;

  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *n, struct stat *st)
{
  int fd;
  int r;

  fd = open(n, O_RDONLY);
  if(fd < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n;

  n = 0;
  while('0' <= *s && *s <= '9')
    n = n*10 + *s++ - '0';
  return n;
}

void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  if (src > dst) {
    while(n-- > 0)
      *dst++ = *src++;
  } else {
    dst += n;
    src += n;
    while(n-- > 0)
      *--dst = *--src;
  }
  return vdst;
}

int
memcmp(const void *s1, const void *s2, uint n)
{
  const char *p1 = s1, *p2 = s2;
  while (n-- > 0) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

char *
sbrk(int n) {
  return __sys_sbrk(n, SBRK_EAGER);
}

char *
sbrklazy(int n) {
  return __sys_sbrk(n, SBRK_LAZY);
}

int
ugetpid(void)
{
  struct usyscall *u = (struct usyscall *)USYSCALL;
  return u->pid;
}
