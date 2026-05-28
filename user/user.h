typedef unsigned long size_t;
typedef long int off_t;

#define SBRK_ERROR ((char *)-1)

struct stat;

// these system calls will be treat as normal func
// ulib.c will invoke the actual syscall
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
int pause(int);
int uptime(void);
int interpose(int, const char*);
int ugetpid(void);
uint64 pgpte(void*);
void kpgtbl(void);
int sigalarm(int, void (*handler)());
int sigreturn(void);
void* mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int bind(uint16);
int unbind(uint16);
int send(uint16, uint32, uint16, char *, uint32);
int recv(uint16, uint32*, uint16*, char *, uint32);
int cpupin(int);
int halt(void);

// raw Linux-numbered syscall stubs
int __sys_getcwd(char*, int);
int __sys_dup(int);
int __sys_dup3(int, int, int);
int __sys_fcntl(int, int, uint64);
int __sys_ioctl(int, uint64, uint64);
int __sys_mknodat(int, const char*, short, short);
int __sys_mkdirat(int, const char*, int);
int __sys_unlinkat(int, const char*, int);
int __sys_linkat(int, const char*, int, const char*, int);
int __sys_renameat(int, const char*, int, const char*);
int __sys_umount2(const char*, int);
int __sys_mount(const char*, const char*, const char*, uint64, const void*);
int __sys_faccessat(int, const char*, int);
int __sys_chdir(const char*);
int __sys_openat(int, const char*, int, int);
int __sys_close(int);
int __sys_pipe2(int*, int);
int __sys_getdents64(int, void*, int);
int __sys_lseek(int, uint64, int);
int __sys_read(int, void*, int);
int __sys_write(int, const void*, int);
int __sys_readv(int, const void*, int);
int __sys_writev(int, const void*, int);
int __sys_pread64(int, void*, int, off_t);
int __sys_sendfile(int, int, off_t*, int);
int __sys_ppoll(void*, uint64, const void*, const void*);
int __sys_readlinkat(int, const char*, char*, int);
int __sys_newfstatat(int, const char*, struct stat*, int);
int __sys_fstat(int, struct stat*);
int __sys_utimensat(int, const char*, const void*, int);
int __sys_clone(uint64, uint64, uint64, uint64, uint64);
int __sys_exit(int) __attribute__((noreturn));
int __sys_exit_group(int) __attribute__((noreturn));
int __sys_set_tid_address(int*);
int __sys_futex(uint64, int, int, uint64, uint64, int);
int __sys_set_robust_list(uint64, uint64);
int __sys_get_robust_list(int, uint64, uint64);
int __sys_nanosleep(const void*, void*);
int __sys_clock_gettime(int, void*);
int __sys_syslog(int, char*, int);
int __sys_sched_yield(void);
int __sys_kill(int, int);
int __sys_rt_sigreturn(void);
int __sys_tkill(int, int);
int __sys_tgkill(int, int, int);
int __sys_rt_sigaction(int, const void*, void*, int);
int __sys_rt_sigprocmask(int, const void*, void*, int);
int __sys_rt_sigtimedwait(const void*, void*, const void*, int);
int __sys_setregid(int, int);
int __sys_setgid(int);
int __sys_setreuid(int, int);
int __sys_setuid(int);
int __sys_times(void*);
int __sys_setsid(void);
int __sys_uname(void*);
int __sys_gettimeofday(void*, void*);
int __sys_getpid(void);
int __sys_getppid(void);
int __sys_getuid(void);
int __sys_geteuid(void);
int __sys_getgid(void);
int __sys_getegid(void);
int __sys_gettid(void);
int __sys_sysinfo(void*);
int __sys_socket(int, int, int);
int __sys_linux_bind(int, const void*, int);
int __sys_listen(int, int);
int __sys_accept(int, void*, int*);
int __sys_connect(int, const void*, int);
int __sys_getsockname(int, void*, int*);
int __sys_sendto(int, const void*, int, int, const void*, int);
int __sys_recvfrom(int, void*, int, int, void*, int*);
int __sys_setsockopt(int, int, int, const void*, int);
uint64 __sys_brk(uint64);
int __sys_munmap(void *, size_t);
int __sys_execve(const char*, char**, char**);
int __sys_halt(void);
void* __sys_mmap(void *, size_t, int, int, int, off_t);
int __sys_mprotect(void*, size_t, int);
int __sys_wait4(int, int*, int, void*);
int __sys_prlimit64(int, int, const void*, void*);
int __sys_statfs(const char*, void*);
int __sys_renameat2(int, const char*, int, const char*, int);
int __sys_getrandom(void*, int, int);

// raw xv6/lab private syscall stubs
char* __sys_sbrk(int,int);
int __sys_pause(int);
int __sys_uptime(void);
int __sys_interpose(int, const char*);
uint64 __sys_pgpte(void*);
void __sys_kpgtbl(void);
int __sys_sigalarm(int, void (*handler)());
int __sys_sigreturn(void);
int __sys_bind(uint16);
int __sys_unbind(uint16);
int __sys_send(uint16, uint32, uint16, char *, uint32);
int __sys_recv(uint16, uint32*, uint16*, char *, uint32);
int __sys_cpupin(int);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char* sbrk(int);
char* sbrklazy(int);

// printf.c
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));

// umalloc.c
void* malloc(uint);
void free(void*);
