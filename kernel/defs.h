typedef unsigned long size_t;
typedef long int off_t;
struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;
struct rwspinlock;
struct vfs_path;
struct vfs_node;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// console.c
void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);

// devzero.c
void            devnullinit(void);
void            devzeroinit(void);

// exec.c
int             kexec(char*, char**);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);

// fs.c
void            fsinit(int);
int             dirlink(struct inode*, char*, uint);
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   ialloc(uint, short);
struct inode*   idup(struct inode*);
void            iinit();
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, int, uint64, uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, int, uint64, uint, uint);
void            itrunc(struct inode*);
void            ireclaim(int);

// kalloc.c
void*           kalloc(void);
void            kfree(void *);
void            kinit(void);
void            kref_inc(uint64);
uint            kref_get(uint64);
void*           superalloc(void);
void            superfree(void *);
void            superref_inc(uint64);
uint            superref_get(uint64);

// log.c
void            initlog(int, struct superblock*);
void            log_write(struct buf*);
void            begin_op(void);
void            end_op(void);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);
int             pipewrite_kernel(struct pipe*, char*, int);

// printf.c
int             printf(char*, ...) __attribute__ ((format (printf, 1, 2)));
void            panic(char*) __attribute__((noreturn));
void            printfinit(void);
void            backtrace(void);

// proc.c
int             cpuid(void);
void            kexit(int);
void            kexit_group(int);
int             kclone(uint64, uint64, uint64, uint64, uint64, int, int, int, int);
int             linux_tgid(struct proc *);
int             linux_reset_thread_group(struct proc *);
void            linux_sync_file_table(struct proc *);
void            linux_futex_wake(uint64);
int             linux_interrupt(int, int, int, int);
void            linux_set_rt_signal_handler(uint64);
void            linux_deliver_signal(void);
uint64          linux_sigreturn(void);
int             linux_take_interrupt(void);
int             growproc(int);
void            proc_mapstacks(pagetable_t);
pagetable_t     proc_pagetable(struct proc *);
void            proc_freepagetable(pagetable_t, uint64, uint64);
int             kkill(int);
int             killed(struct proc*);
void            setkilled(struct proc*);
int             proc_munmap(struct proc *, uint64, uint64);
void            proc_munmapall(struct proc *);
struct cpu*     mycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            sleep(void*, struct spinlock*);
int             futex_timed_sleep(void*, struct spinlock*, uint);
void            futex_set_bitset(uint);
void            futex_tick(uint);
void            userinit(void);
int             kwait(uint64);
int             kwait_options(uint64, int);
int             kwait_linux(uint64);
void            wakeup(void*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);

// vfs.c
void            vfsinit(void);
struct vfs_mount* vfs_root_mount(void);
int             vfs_mount(char*, char*, char*, uint64);
int             vfs_umount(char*, int);
void            vfs_join_path(char*, int, char*, char*);
int             vfs_set_proc_root(struct proc*, char*);
int             vfs_set_proc_cwd(struct proc*, char*);
int             vfs_proc_cwd_path(struct proc*, char*, int);
int             vfs_resolve(char*, struct vfs_path*);
int             vfs_resolve_proc_path(struct proc*, char*, struct vfs_path*);
int             vfs_file_stat_node(struct file*, struct vfs_node*);
int             vfs_file_read_kernel(struct file*, char*, int, uint64);
int             vfs_file_write_kernel(struct file*, char*, int, uint64);

// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);
int             atomic_read4(int *addr);
void            initrwlock(struct rwspinlock *rwlk);
void            read_acquire(struct rwspinlock*);
void            read_release(struct rwspinlock*);
void            write_acquire(struct rwspinlock*);
void            write_release(struct rwspinlock*);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
int             strcmp(const char*, const char*);
char*           strncpy(char*, const char*, int);
char*           strcpy(char*, const char*);

// syscall.c
void            argint(int, int*);
int             argstr(int, char*, int);
void            argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64, uint64*);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct rwspinlock tickslock;
void            prepare_return(void);
void            linux_wall_timespec(uint64*, uint64*);
uint64          linux_nofile_limit(void);

// start.c
void            timerinit(void);

// sbi.c
void            start_other_harts(void);
void            sbi_shutdown(void);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartwrite(char [], int);
void            uartputc_sync(int);
int             uartgetc(void);

// vm.c
void            kvminit(void);
void            kvminithart(void);
void            kvmmap(pagetable_t, uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
uint64          uvmalloc(pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
int             uvmshare(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap_all(pagetable_t);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
pte_t *         walk(pagetable_t, uint64, int);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
int             ismapped(pagetable_t, uint64);
uint64          vmfault(pagetable_t, uint64, int);
void            vmprint(pagetable_t);
pte_t*          pgpte(pagetable_t, uint64);

// plic.c
void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *, int);
void            virtio_disk_intr(void);


// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))

// sprintf.c
int             snprintf(char*, unsigned long, const char*, ...);

#ifdef KCSAN
void            kcsaninit();
#endif

// virtio_net.c
void            virtio_net_init(void);
void            virtio_net_intr(void);
int             virtio_net_transmit(char *, int);

// net.c
void            netinit(void);
void            net_rx(char *buf, int len);
