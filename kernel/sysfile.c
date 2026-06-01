//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "rwlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "vfs.h"

static int linux_copy_stat_time(uint64 staddr, uint64 dev, uint64 ino,
                                uint64 mode, uint64 nlink, uint64 size,
                                uint64 blocks, uint uid, uint gid,
                                uint64 atime, uint64 mtime, uint64 ctime,
                                char *path, struct file *tf);
static int linux_openat_resolve(struct proc *p, int dirfd, char *path,
                                char *out, int outsz);
static int linux_stat_proc_path(struct proc *p, char *path, struct vfs_path *vp,
                                struct vfs_node *node, char *actual,
                                int actualsz);
static void linux_apply_times(uint64 *atime_sec, uint64 *atime_nsec,
                              uint64 *mtime_sec, uint64 *mtime_nsec,
                              int have_ts, uint64 *ts, uint64 now_sec,
                              uint64 now_nsec);
static int vfs_prepare_write_path(struct proc *p, char *path, char *actual,
                                  int actualsz, struct vfs_path *vp);
static int vfs_sync_legacy_cwd(struct proc *p, struct vfs_path *vp);
static void linux_sync_fs_to_group(struct proc *src, struct vfs_path *vp);
extern struct proc proc[NPROC];

static struct spinlock linux_socket_lock;
static int linux_socket_lock_inited;
static ushort linux_next_port = 49152;

#define LINUX_UTIME_SLOTS 64
#define LINUX_UTIME_NOW  ((1U << 30) - 1)
#define LINUX_UTIME_OMIT ((1U << 30) - 2)

struct linux_utime_entry {
  int used;
  char path[MAXPATH];
  uint64 atime_sec;
  uint64 atime_nsec;
  uint64 mtime_sec;
  uint64 mtime_nsec;
};

static struct spinlock linux_utime_lock;
static int linux_utime_lock_inited;
static struct linux_utime_entry linux_utimes[LINUX_UTIME_SLOTS];

static void
linux_socket_lock_init(void)
{
  if(linux_socket_lock_inited == 0){
    initlock(&linux_socket_lock, "linux_socket");
    linux_socket_lock_inited = 1;
  }
}

static void
linux_utime_lock_init(void)
{
  if(linux_utime_lock_inited == 0){
    initlock(&linux_utime_lock, "linux_utime");
    linux_utime_lock_inited = 1;
  }
}

static void
linux_now_timespec(uint64 *sec, uint64 *nsec)
{
  linux_wall_timespec(sec, nsec);
}

static struct linux_utime_entry *
linux_utime_find(char *path, int alloc)
{
  // Look up or allocate the in-memory timestamp override for a path.
  struct linux_utime_entry *free = 0;

  for(int i = 0; i < LINUX_UTIME_SLOTS; i++){
    if(linux_utimes[i].used &&
       strncmp(linux_utimes[i].path, path, MAXPATH) == 0)
      return &linux_utimes[i];
    if(free == 0 && linux_utimes[i].used == 0)
      free = &linux_utimes[i];
  }
  if(alloc && free){
    memset(free, 0, sizeof(*free));
    free->used = 1;
    safestrcpy(free->path, path, MAXPATH);
    return free;
  }
  return 0;
}

static void
linux_share_vma_to_group(struct proc *p, struct vma *v)
{
  (void)p;
  (void)v;
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();
  uint64 limit = linux_nofile_limit();

  for(fd = 0; fd < NOFILE; fd++){
    if((uint64)fd >= limit)
      break;
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      p->ofd_flags[fd] = 0;
      linux_sync_file_table(p);
      return fd;
    }
  }
  return -1;
}

static int
linux_sock_copyin(uint64 addr, uint64 len, struct linux_sockaddr_in *sa)
{
  // Copy a userspace sockaddr_in, accepting short Linux sockaddr lengths.
  memset(sa, 0, sizeof(*sa));
  if(addr == 0 || len < 2)
    return -1;
  if(len > sizeof(*sa))
    len = sizeof(*sa);
  if(copyin(myproc()->pagetable, (char *)sa, addr, len) < 0)
    return -1;
  return 0;
}

static int
linux_sock_copyout(uint64 addr, uint64 lenaddr, struct linux_sockaddr_in *sa)
{
  // Copy sockaddr_in back and update the caller's socklen_t.
  uint64 len = sizeof(*sa);

  if(lenaddr != 0){
    uint len32;
    if(copyin(myproc()->pagetable, (char *)&len32, lenaddr, sizeof(len32)) < 0)
      return -1;
    if(len32 < len)
      len = len32;
    len32 = sizeof(*sa);
    if(copyout(myproc()->pagetable, lenaddr, (char *)&len32, sizeof(len32)) < 0)
      return -1;
  }
  if(addr != 0 && copyout(myproc()->pagetable, addr, (char *)sa, len) < 0)
    return -1;
  return 0;
}

static ushort
linux_sock_port(struct linux_sockaddr_in *sa)
{
  return ((sa->port & 0xff) << 8) | (sa->port >> 8);
}

static ushort
linux_sock_htons(ushort port)
{
  return ((port & 0xff) << 8) | (port >> 8);
}

static ushort
linux_sock_alloc_port(void)
{
  // Allocate simple ephemeral ports from the Linux dynamic range.
  ushort port = linux_next_port++;
  if(linux_next_port < 49152)
    linux_next_port = 49152;
  return port;
}

static struct file *
linux_sock_find_bound(ushort port, int stream)
{
  // Find an in-kernel socket bound to this local port and socket kind.
  extern struct {
    struct spinlock lock;
    struct file file[NFILE];
  } ftable;

  for(struct file *f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref > 0 && f->type == FD_SOCKET &&
       ((f->sock_type & 0xf) == (stream ? 1 : 2)) &&
       linux_sock_port(&f->sock_local) == port)
      return f;
  }
  return 0;
}

uint64
sys_dup(void)
{
  // Duplicate a file descriptor into the lowest free slot.
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return myproc()->is_linux ? -24 : -1;
  filedup(f);
  return fd;
}

uint64
sys_linux_dup3(void)
{
  // Duplicate oldfd into exactly newfd, closing newfd first if needed.
  int oldfd, newfd, flags;
  struct proc *p = myproc();
  struct file *f;

  argint(0, &oldfd);
  argint(1, &newfd);
  argint(2, &flags);

  if(oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE)
    return -9; // EBADF
  if(flags & ~02000000)
    return -22; // EINVAL
  if(oldfd == newfd)
    return -22; // EINVAL
  if((f = p->ofile[oldfd]) == 0)
    return -9; // EBADF
  if(p->ofile[newfd]){
    fileclose(p->ofile[newfd]);
    p->ofile[newfd] = 0;
    p->ofd_flags[newfd] = 0;
  }
  p->ofile[newfd] = filedup(f);
  p->ofd_flags[newfd] = (flags & 02000000) ? 1 : 0; // O_CLOEXEC
  linux_sync_file_table(p);
  return newfd;
}

uint64
sys_linux_fcntl(void)
{
  // Linux fcntl subset for descriptor duplication and fd/status flags.
  int fd, cmd, arg;
  struct proc *p = myproc();
  struct file *f;

  argint(0, &fd);
  argint(1, &cmd);
  argint(2, &arg);

  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return -1;

  switch(cmd){
  case 0:     // F_DUPFD
  case 1030:  // F_DUPFD_CLOEXEC
    if(arg < 0)
      return -1;
    for(int newfd = arg; newfd < NOFILE; newfd++){
      if(p->ofile[newfd] == 0){
        p->ofile[newfd] = filedup(f);
        p->ofd_flags[newfd] = (cmd == 1030) ? 1 : 0;
        linux_sync_file_table(p);
        return newfd;
      }
    }
    return -1;
  case 1:  // F_GETFD
    return p->ofd_flags[fd];
  case 2:  // F_SETFD
    p->ofd_flags[fd] = arg & 1;
    linux_sync_file_table(p);
    return 0;
  case 3:  // F_GETFL
    if(f->readable && f->writable)
      return 2 | f->status_flags; // O_RDWR
    if(f->writable)
      return 1 | f->status_flags; // O_WRONLY
    return f->status_flags;       // O_RDONLY
  case 4:  // F_SETFL
    f->status_flags = arg;
    return 0;
  default:
    return -1;
  }
}

uint64
sys_linux_socket(void)
{
  // Create a minimal AF_INET stream/datagram socket backed by struct file.
  int domain, type, proto;
  struct file *f;
  int fd;

  argint(0, &domain);
  argint(1, &type);
  argint(2, &proto);
  if(domain != 2) // AF_INET
    return -97; // EAFNOSUPPORT
  if((type & 0xf) != 1 && (type & 0xf) != 2)
    return -94; // ESOCKTNOSUPPORT

  if((f = filealloc()) == 0)
    return -24; // EMFILE
  f->type = FD_SOCKET;
  f->readable = 1;
  f->writable = 1;
  f->sock_domain = domain;
  f->sock_type = type;
  f->sock_proto = proto;
  if(type & 04000)
    f->status_flags |= 04000; // O_NONBLOCK
  f->sock_listening = 0;
  f->sock_connected = 0;
  f->sock_pending = 0;
  memset(&f->sock_local, 0, sizeof(f->sock_local));
  memset(&f->sock_peer, 0, sizeof(f->sock_peer));
  f->sock_local.family = 2;
  f->sock_qhead = f->sock_qtail = f->sock_qcount = 0;

  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -24; // EMFILE
  }
  if(type & 02000000){
    myproc()->ofd_flags[fd] = 1; // FD_CLOEXEC
    linux_sync_file_table(myproc());
  }
  return fd;
}

uint64
sys_linux_bind(void)
{
  // Bind a socket to a local IPv4 port; address contents are mostly ignored.
  struct file *f;
  uint64 addr, len;
  struct linux_sockaddr_in sa;

  argaddr(1, &addr);
  argaddr(2, &len);
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if(linux_sock_copyin(addr, len, &sa) < 0)
    return -14; // EFAULT
  if(sa.family != 2)
    return -97; // EAFNOSUPPORT

  linux_socket_lock_init();
  acquire(&linux_socket_lock);
  if(sa.port == 0)
    sa.port = linux_sock_htons(linux_sock_alloc_port());
  if(linux_sock_find_bound(linux_sock_port(&sa), (f->sock_type & 0xf) == 1)){
    release(&linux_socket_lock);
    return -98; // EADDRINUSE
  }
  f->sock_local = sa;
  release(&linux_socket_lock);
  return 0;
}

uint64
sys_linux_getsockname(void)
{
  // Return the socket's local address, auto-assigning a port if needed.
  struct file *f;
  uint64 addr, lenaddr;

  argaddr(1, &addr);
  argaddr(2, &lenaddr);
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if(f->sock_local.family == 0)
    f->sock_local.family = 2;
  if(f->sock_local.port == 0)
    f->sock_local.port = linux_sock_htons(linux_sock_alloc_port());
  if(linux_sock_copyout(addr, lenaddr, &f->sock_local) < 0)
    return -14; // EFAULT
  return 0;
}

uint64
sys_linux_setsockopt(void)
{
  // Stub: accept socket options without storing them.
  return 0;
}

uint64
sys_linux_listen(void)
{
  // Mark a stream socket as accepting connections.
  struct file *f;

  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if((f->sock_type & 0xf) != 1)
    return -95; // EOPNOTSUPP
  if(f->sock_local.port == 0)
    f->sock_local.port = linux_sock_htons(linux_sock_alloc_port());
  f->sock_listening = 1;
  return 0;
}

uint64
sys_linux_connect(void)
{
  // Mark a socket connected and notify a matching in-kernel listener.
  struct file *f, *listener;
  uint64 addr, len;
  struct linux_sockaddr_in sa;

  argaddr(1, &addr);
  argaddr(2, &len);
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if(linux_sock_copyin(addr, len, &sa) < 0)
    return -14; // EFAULT
  f->sock_peer = sa;
  if(f->sock_local.family == 0)
    f->sock_local.family = 2;
  if(f->sock_local.port == 0)
    f->sock_local.port = linux_sock_htons(linux_sock_alloc_port());
  f->sock_connected = 1;

  if((f->sock_type & 0xf) == 1){
    linux_socket_lock_init();
    acquire(&linux_socket_lock);
    listener = linux_sock_find_bound(linux_sock_port(&sa), 1);
    if(listener && listener->sock_listening){
      listener->sock_pending = 1;
      listener->sock_peer = f->sock_local;
      wakeup(listener);
    }
    release(&linux_socket_lock);
  }
  return 0;
}

uint64
sys_linux_accept(void)
{
  // Wait for a pending in-kernel stream connection and return a new socket fd.
  struct file *f, *nf;
  int fd;
  uint64 addr, lenaddr;

  argaddr(1, &addr);
  argaddr(2, &lenaddr);
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if((f->sock_type & 0xf) != 1 || f->sock_listening == 0)
    return -22; // EINVAL

  linux_socket_lock_init();
  acquire(&linux_socket_lock);
  while(f->sock_pending == 0)
    sleep(f, &linux_socket_lock);
  f->sock_pending = 0;
  release(&linux_socket_lock);

  if((nf = filealloc()) == 0)
    return -24; // EMFILE
  nf->type = FD_SOCKET;
  nf->readable = 1;
  nf->writable = 1;
  nf->sock_domain = f->sock_domain;
  nf->sock_type = f->sock_type;
  nf->sock_proto = f->sock_proto;
  nf->sock_local = f->sock_local;
  nf->sock_peer = f->sock_peer;
  nf->sock_connected = 1;
  if((fd = fdalloc(nf)) < 0){
    fileclose(nf);
    return -24; // EMFILE
  }
  if(addr != 0 && linux_sock_copyout(addr, lenaddr, &nf->sock_peer) < 0)
    return -14; // EFAULT
  return fd;
}

uint64
sys_linux_sendto(void)
{
  // Datagram send: enqueue one bounded packet on the destination port.
  struct file *f, *dst;
  uint64 buf, len, addr, addrlen;
  int flags;
  struct linux_sockaddr_in sa;

  argaddr(1, &buf);
  argaddr(2, &len);
  argint(3, &flags);
  argaddr(4, &addr);
  argaddr(5, &addrlen);
  (void)flags;
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF
  if(len > SOCKET_PAYLOAD)
    len = SOCKET_PAYLOAD;
  if(addr != 0){
    if(linux_sock_copyin(addr, addrlen, &sa) < 0)
      return -14;
    f->sock_peer = sa;
  } else {
    sa = f->sock_peer;
  }
  if(f->sock_local.family == 0)
    f->sock_local.family = 2;
  if(f->sock_local.port == 0)
    f->sock_local.port = linux_sock_htons(linux_sock_alloc_port());

  // Deliver to an already bound datagram socket in the local file table.
  linux_socket_lock_init();
  acquire(&linux_socket_lock);
  dst = linux_sock_find_bound(linux_sock_port(&sa), 0);
  if(dst == 0 || dst->sock_qcount >= SOCKET_QUEUE){
    release(&linux_socket_lock);
    return -111; // ECONNREFUSED
  }
  struct linux_sockpkt *pkt = &dst->sock_q[dst->sock_qtail];
  pkt->len = len;
  pkt->from = f->sock_local;
  if(copyin(myproc()->pagetable, pkt->data, buf, len) < 0){
    release(&linux_socket_lock);
    return -14;
  }
  dst->sock_qtail = (dst->sock_qtail + 1) % SOCKET_QUEUE;
  dst->sock_qcount++;
  wakeup(dst);
  release(&linux_socket_lock);
  return len;
}

uint64
sys_linux_recvfrom(void)
{
  // Datagram receive: block until a packet is queued, then copy data/source.
  struct file *f;
  uint64 buf, len, addr, addrlen;
  int flags;

  argaddr(1, &buf);
  argaddr(2, &len);
  argint(3, &flags);
  argaddr(4, &addr);
  argaddr(5, &addrlen);
  (void)flags;
  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET)
    return -9; // EBADF

  linux_socket_lock_init();
  acquire(&linux_socket_lock);
  while(f->sock_qcount == 0)
    sleep(f, &linux_socket_lock);
  struct linux_sockpkt pkt = f->sock_q[f->sock_qhead];
  f->sock_qhead = (f->sock_qhead + 1) % SOCKET_QUEUE;
  f->sock_qcount--;
  release(&linux_socket_lock);

  if(len > pkt.len)
    len = pkt.len;
  if(copyout(myproc()->pagetable, buf, pkt.data, len) < 0)
    return -14;
  if(addr != 0 && linux_sock_copyout(addr, addrlen, &pkt.from) < 0)
    return -14;
  return len;
}

uint64
sys_read(void)
{
  // Read from an open file descriptor into user memory.
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  // Write from user memory to an open file descriptor.
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  // Close an xv6 file descriptor and drop its file reference.
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  myproc()->ofd_flags[fd] = 0;
  linux_sync_file_table(myproc());
  fileclose(f);
  return 0;
}

uint64
sys_linux_close(void)
{
  // Linux close with EBADF-style error for invalid descriptors.
  int fd;
  struct file *f;

  struct proc *p = myproc();
  argint(0, &fd);
  if(argfd(0, &fd, &f) < 0){
    return -9; // EBADF
  }
  p->ofile[fd] = 0;
  p->ofd_flags[fd] = 0;
  linux_sync_file_table(p);
  fileclose(f);
  return 0;
}

static int
linux_vma_slot(struct proc *p)
{
  struct linux_mm *mm = p->mm;
  for(int i = 0; i < NVMA; i++){
    if(mm->vmas[i].used == 0)
      return i;
  }
  return -1;
}

static int
linux_vma_overlaps(struct proc *p, uint64 addr, uint64 len)
{
  struct linux_mm *mm = p->mm;
  uint64 end = addr + len;

  if(end < addr)
    return 1;
  for(int i = 0; i < NVMA; i++){
    if(mm->vmas[i].used == 0)
      continue;
    uint64 vstart = mm->vmas[i].addr;
    uint64 vend = vstart + mm->vmas[i].len;
    if(addr < vend && end > vstart)
      return 1;
  }
  return 0;
}

static int
linux_mmap_addr_ok(struct proc *p, uint64 addr, uint64 len)
{
  struct linux_mm *mm = p->mm;
  if(len == 0 || addr >= MMAP_TOP || addr + len < addr || addr + len > MMAP_TOP)
    return 0;
  if(addr < mm->sz)
    return 0;
  if(addr < PGSIZE)
    return 0;
  return 1;
}

static uint64
linux_mmap_pick_addr(struct proc *p, uint64 hint, uint64 len)
{
  struct linux_mm *mm = p->mm;
  uint64 addr;

  if(hint != 0){
    hint = PGROUNDDOWN(hint);
    if(linux_mmap_addr_ok(p, hint, len) && linux_vma_overlaps(p, hint, len) == 0)
      return hint;
  }

  addr = PGROUNDDOWN(mm->mmap_base - len);
  while(linux_mmap_addr_ok(p, addr, len)){
    if(linux_vma_overlaps(p, addr, len) == 0)
      return addr;
    if(addr < len + PGSIZE)
      break;
    addr = PGROUNDDOWN(addr - len);
  }
  return 0;
}

static uint64
linux_mmap_create(uint64 addr, uint64 len, int prot, int flags, int fd,
                  uint64 offset, int linux_abi)
{
  struct proc *p = myproc();
  struct linux_mm *mm = p->mm;
  struct file *f = 0;
  struct vfs_node node;
  int map_type = flags & (MAP_SHARED | MAP_PRIVATE);
  int anonymous = flags & MAP_ANONYMOUS;
  uint64 maplen;
  uint64 mapaddr;
  int slot;

  if(len == 0)
    return linux_abi ? -22 : -1; // EINVAL
  if(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return linux_abi ? -22 : -1; // EINVAL
  if(map_type != MAP_SHARED && map_type != MAP_PRIVATE)
    return linux_abi ? -22 : -1; // EINVAL
  if((offset % PGSIZE) != 0)
    return linux_abi ? -22 : -1; // EINVAL

  maplen = PGROUNDUP(len);
  if(maplen == 0 || maplen < len)
    return linux_abi ? -12 : -1; // ENOMEM

  if(anonymous == 0){
    if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
      return linux_abi ? -9 : -1; // EBADF
    if(vfs_file_stat_node(f, &node) < 0 || node.type != T_FILE)
      return linux_abi ? -13 : -1; // EACCES
    if((prot & PROT_READ) && f->readable == 0)
      return linux_abi ? -13 : -1; // EACCES
    if((flags & MAP_SHARED) && (prot & PROT_WRITE) && f->writable == 0)
      return linux_abi ? -13 : -1; // EACCES
  } else {
    memset(&node, 0, sizeof(node));
  }

  if(flags & MAP_FIXED){
    if((addr % PGSIZE) != 0 || linux_mmap_addr_ok(p, addr, maplen) == 0)
      return linux_abi ? -22 : -1; // EINVAL
    if(linux_vma_overlaps(p, addr, maplen) && proc_munmap(p, addr, maplen) < 0)
      return linux_abi ? -12 : -1; // ENOMEM
    mapaddr = addr;
  } else {
    mapaddr = linux_mmap_pick_addr(p, addr, maplen);
    if(mapaddr == 0)
      return linux_abi ? -12 : -1; // ENOMEM
  }

  slot = linux_vma_slot(p);
  if(slot < 0)
    return linux_abi ? -12 : -1; // ENOMEM

  if(mapaddr < mm->mmap_base)
    mm->mmap_base = mapaddr;
  mm->vmas[slot].used = 1;
  mm->vmas[slot].addr = mapaddr;
  mm->vmas[slot].len = maplen;
  mm->vmas[slot].filelen = anonymous ? 0 : node.size;
  mm->vmas[slot].prot = prot;
  mm->vmas[slot].flags = flags;
  mm->vmas[slot].offset = offset;
  mm->vmas[slot].f = anonymous ? 0 : filedup(f);
  linux_share_vma_to_group(p, &mm->vmas[slot]);

  return mapaddr;
}

uint64
sys_mmap(void)
{
  // xv6 mmap: create a lazy file-backed VMA at the next downward address.
  uint64 addr;
  uint64 len;
  int prot, flags;
  uint64 offset;
  int fd;

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &offset);
  return linux_mmap_create(addr, len, prot, flags, fd, offset, 0);
}

uint64
sys_linux_mmap(void)
{
  // Linux mmap subset: anonymous mappings here, file mappings via sys_mmap().
  uint64 addr;
  uint64 len;
  int prot, flags, fd;
  uint64 offset;

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &offset);
  return linux_mmap_create(addr, len, prot, flags, fd, offset, 1);
}

uint64
sys_munmap(void)
{
  // Remove a mapped VMA range and write back shared dirty file pages.
  uint64 addr, len;
  struct proc *p = myproc();
  argaddr(0, &addr);
  argaddr(1, &len);

  if(len == 0)
    return 0;
  if((addr % PGSIZE) != 0 || addr + len < addr)
    return p->is_linux ? -22 : -1; // EINVAL
  if(proc_munmap(p, addr, PGROUNDUP(len)) < 0)
    return p->is_linux ? -22 : -1; // EINVAL
  return 0;
}

uint64
sys_linux_madvise(void)
{
  // Keep mmap advice as a compatibility no-op.  libc uses this on thread
  // stacks and arenas; ignoring the hint preserves the mapping contents.
  uint64 addr, len;
  int advice;

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &advice);

  if(len == 0)
    return 0;
  if((addr % PGSIZE) != 0 || addr + len < addr)
    return -22; // EINVAL
  if(advice < 0 || advice > 28)
    return -22; // EINVAL
  return 0;
}

static int
linux_mprotect_one(struct proc *p, uint64 start, uint64 end, int prot)
{
  // Change protection for a subrange of one tracked VMA, splitting if needed.
  struct linux_mm *mm = p->mm;

  for(int i = 0; i < NVMA; i++){
    if(mm->vmas[i].used == 0)
      continue;
    if(start >= mm->vmas[i].addr &&
       end <= mm->vmas[i].addr + mm->vmas[i].len){
      struct vma old = mm->vmas[i];
      int need = 0;
      if(start > old.addr)
        need++;
      if(end < old.addr + old.len)
        need++;
      // Reserve slots for left/right fragments before mutating the VMA table.
      int slots[2];
      for(int j = 0; j < need; j++){
        slots[j] = -1;
        for(int k = 0; k < NVMA; k++){
          if(mm->vmas[k].used == 0){
            slots[j] = k;
            mm->vmas[k].used = -1;
            break;
          }
        }
        if(slots[j] < 0){
          for(int k = 0; k < j; k++)
            mm->vmas[slots[k]].used = 0;
          return -12; // ENOMEM
        }
      }
      for(int j = 0; j < need; j++)
        mm->vmas[slots[j]].used = 0;

      mm->vmas[i] = old;
      mm->vmas[i].addr = start;
      mm->vmas[i].len = end - start;
      mm->vmas[i].offset = old.offset + (start - old.addr);
      mm->vmas[i].prot = prot;

      int s = 0;
      if(start > old.addr){
        mm->vmas[slots[s]] = old;
        mm->vmas[slots[s]].addr = old.addr;
        mm->vmas[slots[s]].len = start - old.addr;
        if(mm->vmas[slots[s]].f)
          filedup(mm->vmas[slots[s]].f);
        s++;
      }
      if(end < old.addr + old.len){
        mm->vmas[slots[s]] = old;
        mm->vmas[slots[s]].addr = end;
        mm->vmas[slots[s]].len = old.addr + old.len - end;
        mm->vmas[slots[s]].offset = old.offset + (end - old.addr);
        if(mm->vmas[slots[s]].f)
          filedup(mm->vmas[slots[s]].f);
      }
      // Already-faulted pages need their PTE permissions updated immediately.
      for(uint64 a = start; a < end; a += PGSIZE){
        pte_t *pte = walk(p->pagetable, a, 0);
        if(pte == 0 || (*pte & PTE_V) == 0)
          continue;
        if(prot == 0){
          uvmunmap(p->pagetable, a, 1, 1);
          continue;
        }
        uint64 pa = PTE2PA(*pte);
        int perm = PTE_U;
        if(prot & PROT_READ)
          perm |= PTE_R;
        if(prot & PROT_WRITE)
          perm |= PTE_W;
        if(prot & PROT_EXEC)
          perm |= PTE_X;
        *pte = PA2PTE(pa) | perm | PTE_V;
      }
      return 0;
    }
  }
  return -12; // ENOMEM
}

static int
linux_mprotect_mapped_pages(struct proc *p, uint64 start, uint64 end, int prot)
{
  // Fallback for ELF loader segments that are mapped but not recorded as VMAs.
  int perm = PTE_U;

  if(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -22; // EINVAL
  if(prot & PROT_READ)
    perm |= PTE_R;
  if(prot & PROT_WRITE)
    perm |= PTE_W;
  if(prot & PROT_EXEC)
    perm |= PTE_X;

  for(uint64 a = start; a < end; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -12; // ENOMEM
  }

  for(uint64 a = start; a < end; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    uint64 pa = PTE2PA(*pte);
    *pte = PA2PTE(pa) | perm | PTE_V;
  }
  return 0;
}

uint64
sys_linux_mprotect(void)
{
  // Linux mprotect: update VMA metadata and current PTE permissions.
  uint64 addr, len;
  int prot;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  if(len == 0)
    return 0;
  if((addr % PGSIZE) != 0 || addr + len < addr)
    return -22; // EINVAL
  if(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -22; // EINVAL

  uint64 start = addr;
  uint64 end = PGROUNDUP(addr + len);
  if(linux_mprotect_one(p, start, end, prot) < 0 &&
     linux_mprotect_mapped_pages(p, start, end, prot) < 0)
    return -12; // ENOMEM

  return 0;
}

uint64
sys_fstat(void)
{
  // Stat an open xv6 file descriptor.
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  // Create a hard link; both paths must resolve to the same mounted FS.
  char new[MAXPATH], old[MAXPATH], actual_old[MAXPATH], actual_new[MAXPATH];
  struct vfs_path oldvp, newvp;
  struct proc *p = myproc();

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, old, actual_old, sizeof(actual_old), &oldvp) < 0 ||
     vfs_prepare_write_path(p, new, actual_new, sizeof(actual_new), &newvp) < 0)
    return -1;
  if(oldvp.mount != newvp.mount ||
     oldvp.ops == 0 || oldvp.ops->inode == 0 || oldvp.ops->inode->link == 0 ||
     oldvp.ops->inode->link(oldvp.mount, oldvp.inner, newvp.inner) < 0)
    return -1;
  return 0;
}

uint64
sys_unlink(void)
{
  // Remove a filesystem entry through the process's real VFS root/cwd.
  char path[MAXPATH], actual[MAXPATH];
  struct vfs_path vp;
  struct proc *p = myproc();

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->unlink == 0 ||
     vp.ops->inode->unlink(vp.mount, vp.inner) < 0)
    return -1;
  return 0;
}

static int
linux_stat_proc_path(struct proc *p, char *path, struct vfs_path *vp,
                     struct vfs_node *node, char *actual, int actualsz)
{
  struct vfs_path tmp;

  if(p == 0 || path == 0 || node == 0)
    return -2; // ENOENT

  if(vp == 0){
    vp = &tmp;
  }
  if(vfs_resolve_proc_path(p, path, vp) == 0 &&
     vp->ops && vp->ops->inode && vp->ops->inode->lookup &&
     vp->ops->inode->lookup(vp->mount, vp->inner, node) == 0){
    if(actual && actualsz > 0)
      safestrcpy(actual, vp->abs_path, actualsz);
    return 0;
  }

  return -2; // ENOENT
}

static void
linux_apply_times(uint64 *atime_sec, uint64 *atime_nsec,
                  uint64 *mtime_sec, uint64 *mtime_nsec, int have_ts,
                  uint64 *ts, uint64 now_sec, uint64 now_nsec)
{
  if(have_ts == 0){
    *atime_sec = now_sec;
    *atime_nsec = now_nsec;
    *mtime_sec = now_sec;
    *mtime_nsec = now_nsec;
    return;
  }

  if(ts[1] != LINUX_UTIME_OMIT){
    if(ts[1] == LINUX_UTIME_NOW){
      *atime_sec = now_sec;
      *atime_nsec = now_nsec;
    } else {
      *atime_sec = ts[0];
      *atime_nsec = ts[1];
    }
  }
  if(ts[3] != LINUX_UTIME_OMIT){
    if(ts[3] == LINUX_UTIME_NOW){
      *mtime_sec = now_sec;
      *mtime_nsec = now_nsec;
    } else {
      *mtime_sec = ts[2];
      *mtime_nsec = ts[3];
    }
  }
}

static int
vfs_prepare_write_path(struct proc *p, char *path, char *actual, int actualsz,
                       struct vfs_path *vp)
{
  // Ext4 is writable now; write paths should resolve to the process's real
  // root/cwd instead of the old xv6 /tmp redirect layer.

  if(p == 0 || path == 0 || actual == 0 || actualsz <= 0 || vp == 0)
    return -1;

  safestrcpy(actual, path, actualsz);
  return vfs_resolve_proc_path(p, path, vp);
}

static int
vfs_sync_legacy_cwd(struct proc *p, struct vfs_path *vp)
{
  // Keep p->cwd valid for old xv6 filesystem code when cwd is on xv6 FS.
  struct inode *ip;

  if(p == 0 || vp == 0)
    return -1;

  if(vp->type == VFS_EXT4)
    return 0;

  if(vp->type != VFS_XV6)
    return -1;

  begin_op();
  if((ip = namei(vp->inner)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  if(p->cwd)
    iput(p->cwd);
  end_op();

  p->cwd = ip;
  return 0;
}

static void
linux_sync_fs_to_group(struct proc *src, struct vfs_path *vp)
{
  // CLONE_FS threads share cwd/root state, so propagate chdir changes.
  if(src == 0 || src->linux_share_fs == 0)
    return;

  for(struct proc *q = proc; q < &proc[NPROC]; q++){
    if(q == src || q->state == UNUSED || q->linux_share_fs == 0)
      continue;
    if(linux_tgid(q) != linux_tgid(src))
      continue;
    vfs_set_proc_cwd(q, src->vfs_cwd.abs_path);
    vfs_sync_legacy_cwd(q, vp);
  }
}

uint64
sys_open(void)
{
  // Open or create a file through VFS.
  char path[MAXPATH];
  char actual[MAXPATH];
  int fd, omode;
  struct file *f;
  struct vfs_path vp;
  struct vfs_node node;
  struct proc *p = myproc();
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  if((omode & O_CREATE) == 0){
    if(vfs_resolve_proc_path(p, path, &vp) == 0 &&
       vp.ops && vp.ops->file && vp.ops->file->open &&
       vp.ops->file->open(vp.mount, vp.inner, omode, &f) == 0){
      if((fd = fdalloc(f)) < 0){
        fileclose(f);
        return -24; // EMFILE
      }
      return fd;
    }
    return -1;
  }

  // Creates always go through the write-path resolver.
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->create == 0 ||
     vp.ops->file == 0 || vp.ops->file->open == 0 ||
     vp.ops->inode->create(vp.mount, vp.inner, T_FILE, 0, &node) < 0 ||
     vp.ops->file->open(vp.mount, vp.inner, omode & ~O_CREATE, &f) < 0)
    return -1;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -24; // EMFILE
  }
  return fd;
}

static int
linux_open_flags(int flags)
{
  // Translate the small Linux open flag subset used by the compat layer.
  int omode = flags & 3;
  if(flags & 0x40)   // O_CREAT
    omode |= O_CREATE;
  if(flags & 0x200)  // O_TRUNC
    omode |= O_TRUNC;
  if(flags & 0x400)  // O_APPEND
    omode |= O_APPEND;
  return omode;
}

static int
linux_openat_resolve(struct proc *p, int dirfd, char *path, char *out, int outsz)
{
  char base[MAXPATH];
  struct file *df;

  if(p == 0 || path == 0 || out == 0 || outsz <= 0)
    return -1;
  if(path[0] == '/' || dirfd == -100){
    safestrcpy(out, path, outsz);
    return 0;
  }
  if(dirfd < 0 || dirfd >= NOFILE || (df = p->ofile[dirfd]) == 0)
    return -1;

  if(df->type == FD_EXT4){
    if(p->vfs_root.mount && p->vfs_root.mount->type == VFS_EXT4)
      safestrcpy(base, df->ext4_path, sizeof(base));
    else
      snprintf(base, sizeof(base), "/ext4%s", df->ext4_path);
    vfs_join_path(out, outsz, base, path);
    return 0;
  }
  if(df->type == FD_INODE && df->ip && df->ip->inum == ROOTINO){
    vfs_join_path(out, outsz, "/", path);
    return 0;
  }
  return -1;
}

uint64
sys_linux_openat(void)
{
  // Linux openat subset with directory-fd relative path handling.
  char path[MAXPATH];
  char opath[MAXPATH];
  char actual[MAXPATH];
  int fd, dirfd, flags, omode;
  uint64 mode;
  struct file *f;
  struct vfs_path vp;
  struct vfs_node node;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argint(2, &flags);
  argaddr(3, &mode);
  if(argstr(1, path, MAXPATH) < 0)
    return -14; // EFAULT
  if(linux_openat_resolve(p, dirfd, path, opath, sizeof(opath)) < 0)
    return -9; // EBADF
  int has_fd = 0;
  for(int i = 0; i < NOFILE && (uint64)i < linux_nofile_limit(); i++){
    if(p->ofile[i] == 0){
      has_fd = 1;
      break;
    }
  }
  if(has_fd == 0)
    return -24; // EMFILE
  omode = linux_open_flags(flags);
  if((omode & O_CREATE) == 0){
    if(vfs_resolve_proc_path(p, opath, &vp) == 0 &&
       vp.ops && vp.ops->file && vp.ops->file->open &&
       vp.ops->file->open(vp.mount, vp.inner, omode, &f) == 0){
      if((fd = fdalloc(f)) < 0){
        fileclose(f);
        return -24; // EMFILE
      }
      return fd;
    }
    return -2; // ENOENT
  }

  if(vfs_prepare_write_path(p, opath, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->create == 0 ||
     vp.ops->file == 0 || vp.ops->file->open == 0 ||
     vp.ops->inode->create(vp.mount, vp.inner, T_FILE, mode, &node) < 0 ||
     vp.ops->file->open(vp.mount, vp.inner, omode & ~O_CREATE, &f) < 0)
    return -2; // ENOENT

  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -24; // EMFILE
  }
  return fd;
}

uint64
sys_linux_writev(void)
{
  // Write each iovec in order, returning bytes written before any failure.
  struct file *f;
  int fd, iovcnt;
  uint64 iov;
  uint64 base, len;
  int total = 0;

  argint(0, &fd);
  argaddr(1, &iov);
  argint(2, &iovcnt);
  if(argfd(0, 0, &f) < 0)
    return -9; // EBADF
  if(iovcnt < 0 || iovcnt > 16)
    return -22; // EINVAL

  for(int i = 0; i < iovcnt; i++){
    if(fetchaddr(iov + i * 16, &base) < 0 ||
       fetchaddr(iov + i * 16 + 8, &len) < 0)
      return -14; // EFAULT
    if(len > 0x7fffffff)
      return -22; // EINVAL
    int n = filewrite(f, base, len);
    if(n < 0)
      return total > 0 ? total : -9; // EBADF
    total += n;
  }
  return total;
}

uint64
sys_linux_readv(void)
{
  // Read into each iovec in order, stopping on short read.
  struct file *f;
  int fd, iovcnt;
  uint64 iov;
  uint64 base, len;
  int total = 0;

  argint(0, &fd);
  argaddr(1, &iov);
  argint(2, &iovcnt);
  if(argfd(0, 0, &f) < 0)
    return -9; // EBADF
  if(iovcnt < 0 || iovcnt > 16)
    return -22; // EINVAL

  for(int i = 0; i < iovcnt; i++){
    if(fetchaddr(iov + i * 16, &base) < 0 ||
       fetchaddr(iov + i * 16 + 8, &len) < 0)
      return -14; // EFAULT
    if(len > 0x7fffffff)
      return -22; // EINVAL
    int n = fileread(f, base, len);
    if(n < 0)
      return total > 0 ? total : -9; // EBADF
    total += n;
    if(n < len)
      break;
  }
  return total;
}

uint64
sys_linux_lseek(void)
{
  // Dispatch lseek to the backing VFS file operation.
  int fd, whence;
  uint64 off;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &off);
  argint(2, &whence);
  if(argfd(0, 0, &f) < 0)
    return -9; // EBADF

  if(f->vfs_ops && f->vfs_ops->file && f->vfs_ops->file->lseek)
    return f->vfs_ops->file->lseek(f, off, whence);
  return -29; // ESPIPE
}

uint64
sys_linux_pread64(void)
{
  // Positional read implemented by temporarily swapping the file offset.
  struct file *f;
  uint64 buf, off;
  int fd, n;
  uint64 oldoff;
  int r;

  argint(0, &fd);
  argaddr(1, &buf);
  argint(2, &n);
  argaddr(3, &off);
  if(argfd(0, 0, &f) < 0)
    return -9; // EBADF
  if(n < 0)
    return -22; // EINVAL

  oldoff = f->off;
  f->off = off;
  r = fileread(f, buf, n);
  f->off = oldoff;
  return r;
}

uint64
sys_linux_statfs(void)
{
  // Return a fixed statfs structure sufficient for Linux userland probes.
  uint64 buf;
  char path[MAXPATH];
  uint64 st[15];

  if(argstr(0, path, MAXPATH) < 0)
    return -14; // EFAULT
  argaddr(1, &buf);
  memset(st, 0, sizeof(st));
  st[0] = 0x10203040; // f_type
  st[1] = BSIZE;      // f_bsize
  st[2] = 10000;      // f_blocks
  st[3] = 5000;       // f_bfree
  st[4] = 5000;       // f_bavail
  st[5] = 200;        // f_files
  st[6] = 100;        // f_ffree
  st[8] = DIRSIZ;     // f_namelen
  st[9] = BSIZE;      // f_frsize
  if(copyout(myproc()->pagetable, buf, (char *)st, sizeof(st)) < 0)
    return -14; // EFAULT
  return 0;
}

uint64
sys_linux_sendfile(void)
{
  // Copy bytes from a regular input file into a pipe without user bounce.
  int outfd, infd;
  uint64 offaddr, count;
  uint64 off;
  uint64 done = 0;
  int err = 0;
  struct proc *p = myproc();
  struct file *outf, *inf;
  struct vfs_node node;
  char *buf;

  argint(0, &outfd);
  argint(1, &infd);
  argaddr(2, &offaddr);
  argaddr(3, &count);

  if(outfd < 0 || outfd >= NOFILE || infd < 0 || infd >= NOFILE)
    return -9; // EBADF
  if((outf = p->ofile[outfd]) == 0 || (inf = p->ofile[infd]) == 0)
    return -9; // EBADF
  if(outf->type != FD_PIPE || outf->writable == 0)
    return -22; // EINVAL
  if(vfs_file_stat_node(inf, &node) < 0 || node.type != T_FILE)
    return -22; // EINVAL
  if(offaddr != 0){
    if(fetchaddr(offaddr, &off) < 0)
      return -14; // EFAULT
  } else {
    off = inf->off;
  }

  buf = kalloc();
  if(buf == 0)
    return -12; // ENOMEM

  // Move data in page-sized chunks through a temporary kernel buffer.
  while(done < count){
    int n = count - done;
    int r, w;

    if(n > PGSIZE)
      n = PGSIZE;
    r = vfs_file_read_kernel(inf, buf, n, off);
    if(r <= 0)
      break;

    w = pipewrite_kernel(outf->pipe, buf, r);
    if(w < 0){
      if(done == 0)
        err = -32; // EPIPE
      break;
    }
    done += w;
    off += w;
    if(w < r)
      break;
  }

  kfree(buf);
  if(done > 0){
    if(offaddr != 0)
      copyout(p->pagetable, offaddr, (char *)&off, sizeof(off));
    else
      inf->off = off;
  }
  if(err)
    return err;
  return done;
}

uint64
sys_linux_ppoll(void)
{
  // Minimal poll: report existing descriptors as ready without sleeping.
  uint64 fds;
  int nfds;
  int ready = 0;
  struct proc *p = myproc();

  argaddr(0, &fds);
  argint(1, &nfds);
  if(nfds < 0 || nfds > 64)
    return -22; // EINVAL

  for(int i = 0; i < nfds; i++){
    uint64 addr = fds + i * 8;
    int fd;
    short events;
    short revents = 0;

    if(copyin(p->pagetable, (char *)&fd, addr, sizeof(fd)) < 0 ||
       copyin(p->pagetable, (char *)&events, addr + 4, sizeof(events)) < 0)
      return -14; // EFAULT
    if(fd >= 0 && fd < NOFILE && p->ofile[fd]){
      revents = events & 0x5; // POLLIN | POLLOUT
      if(revents == 0)
        revents = 0x1;
      ready++;
    }
    if(copyout(p->pagetable, addr + 6, (char *)&revents, sizeof(revents)) < 0)
      return -14; // EFAULT
  }
  return ready;
}

uint64
sys_linux_newfstatat(void)
{
  // Linux stat by path through the process's real VFS root/cwd.
  char path[MAXPATH];
  char actual[MAXPATH];
  uint64 staddr;
  int dirfd, flags;
  struct vfs_path vp;
  struct vfs_node node;
  uint64 mode;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argaddr(2, &staddr);
  argint(3, &flags);
  (void)flags;
  if(argstr(1, path, MAXPATH) < 0)
    return -14; // EFAULT
  if(path[0] == 0){
    struct file *f;

    if((flags & 0x1000) == 0)
      return -2; // ENOENT
    if(dirfd < 0 || dirfd >= NOFILE || (f = p->ofile[dirfd]) == 0)
      return -9; // EBADF
    if(vfs_file_stat_node(f, &node) < 0)
      return -9; // EBADF
    mode = node.mode | 0777;
    if(mode == 0777){
      if(node.type == T_DIR)
        mode = 0040000 | 0777;
      else if(node.type == T_DEVICE)
        mode = 0020000 | 0777;
      else
        mode = 0100000 | 0777;
    }
    return linux_copy_stat_time(staddr, FIRSTDEV, node.ino, mode, node.nlink,
                                node.size, node.blocks, node.uid, node.gid,
                                node.atime, node.mtime, node.ctime, 0, f);
  }
  (void)dirfd;

  if(linux_stat_proc_path(p, path, &vp, &node, actual, sizeof(actual)) < 0)
    return -2; // ENOENT

  mode = node.mode | 0777;
  if(mode == 0777){
    if(node.type == T_DIR)
      mode = 0040000 | 0777;
    else if(node.type == T_DEVICE)
      mode = 0020000 | 0777;
    else
      mode = 0100000 | 0777;
  }
  return linux_copy_stat_time(staddr, FIRSTDEV, node.ino, mode, node.nlink,
                              node.size, node.blocks, node.uid, node.gid,
                              node.atime, node.mtime, node.ctime, actual, 0);
}

uint64
sys_linux_utimensat(void)
{
  // Store Linux atime/mtime updates in memory after resolving the target.
  int dirfd, flags;
  uint64 pathaddr, times;
  char path[MAXPATH];
  char actual[MAXPATH];
  uint64 now_sec, now_nsec;
  uint64 ts[4];
  int have_ts = 0;
  struct file *f = 0;
  struct vfs_node node;
  struct proc *p = myproc();
  uint64 atime_sec = 0, atime_nsec = 0, mtime_sec = 0, mtime_nsec = 0;

  argint(0, &dirfd);
  argaddr(1, &pathaddr);
  argaddr(2, &times);
  argint(3, &flags);
  (void)flags;

  if(pathaddr == 0){
    if(dirfd < 0 || dirfd >= NOFILE || (f = p->ofile[dirfd]) == 0)
      return -9; // EBADF
    if(f->ext4_path[0])
      safestrcpy(actual, f->ext4_path, sizeof(actual));
    else
      actual[0] = 0;
  } else {
    if(argstr(1, path, MAXPATH) < 0)
      return -14; // EFAULT
    if(path[0] == 0)
      return -2; // ENOENT
    if(linux_stat_proc_path(p, path, 0, &node, actual, sizeof(actual)) < 0){
      char prefix[MAXPATH];
      char *slash = 0;
      for(char *s = path; *s; s++){
        if(*s == '/')
          slash = s;
      }
      if(slash && slash != path){
        int n = slash - path;
        if(n >= sizeof(prefix))
          n = sizeof(prefix) - 1;
        memmove(prefix, path, n);
        prefix[n] = 0;
        if(linux_stat_proc_path(p, prefix, 0, &node, actual, sizeof(actual)) == 0 &&
           node.type != T_DIR)
          return -20; // ENOTDIR
      }
      return -2; // ENOENT
    }
  }

  linux_now_timespec(&now_sec, &now_nsec);
  if(times != 0){
    if(copyin(myproc()->pagetable, (char *)ts, times, sizeof(ts)) < 0){
      return -14; // EFAULT
    }
    have_ts = 1;
  }

  if(f){
    // fd-based update: attach timestamps to this open file and optional path.
    f->has_time = 1;
    atime_sec = f->atime_sec;
    atime_nsec = f->atime_nsec;
    mtime_sec = f->mtime_sec;
    mtime_nsec = f->mtime_nsec;
    linux_apply_times(&atime_sec, &atime_nsec, &mtime_sec, &mtime_nsec,
                      have_ts, ts, now_sec, now_nsec);
    f->atime_sec = atime_sec;
    f->atime_nsec = atime_nsec;
    f->mtime_sec = mtime_sec;
    f->mtime_nsec = mtime_nsec;
    if(actual[0]){
      linux_utime_lock_init();
      acquire(&linux_utime_lock);
      struct linux_utime_entry *e = linux_utime_find(actual, 1);
      if(e){
        e->atime_sec = f->atime_sec;
        e->atime_nsec = f->atime_nsec;
        e->mtime_sec = f->mtime_sec;
        e->mtime_nsec = f->mtime_nsec;
      }
      release(&linux_utime_lock);
    }
    return 0;
  }

  // path-based update: remember timestamps in the global small override table.
  linux_utime_lock_init();
  acquire(&linux_utime_lock);
  struct linux_utime_entry *e = linux_utime_find(actual, 1);
  if(e == 0){
    release(&linux_utime_lock);
    return -12; // ENOMEM
  }

  linux_apply_times(&e->atime_sec, &e->atime_nsec, &e->mtime_sec,
                    &e->mtime_nsec, have_ts, ts, now_sec, now_nsec);
  release(&linux_utime_lock);
  return 0;
}

static int
linux_copy_stat_time(uint64 staddr, uint64 dev, uint64 ino,
                     uint64 mode, uint64 nlink, uint64 size, uint64 blocks,
                     uint uid, uint gid, uint64 atime, uint64 mtime,
                     uint64 ctime, char *path, struct file *tf)
{
  // Build the riscv64 Linux stat layout used by glibc.
  char st[128];
  uint mode32 = mode;
  uint nlink32 = nlink;
  uint uid32 = uid;
  uint gid32 = gid;
  uint64 atime_sec = atime, atime_nsec = 0;
  uint64 mtime_sec = mtime, mtime_nsec = 0;
  uint64 ctime_sec = ctime, ctime_nsec = 0;
  uint64 blksize = BSIZE;

  memset(st, 0, sizeof(st));
  if(atime_sec == 0 && mtime_sec == 0 && ctime_sec == 0){
    linux_wall_timespec(&mtime_sec, &mtime_nsec);
    atime_sec = mtime_sec;
    atime_nsec = mtime_nsec;
    ctime_sec = mtime_sec;
    ctime_nsec = mtime_nsec;
  }
  if(tf && tf->has_time){
    // Prefer timestamps stored on the open file.
    atime_sec = tf->atime_sec;
    atime_nsec = tf->atime_nsec;
    mtime_sec = tf->mtime_sec;
    mtime_nsec = tf->mtime_nsec;
    ctime_sec = mtime_sec;
    ctime_nsec = mtime_nsec;
  } else if(path){
    // Otherwise use path timestamp overrides from utimensat().
    linux_utime_lock_init();
    acquire(&linux_utime_lock);
    struct linux_utime_entry *e = linux_utime_find(path, 0);
    if(e){
      atime_sec = e->atime_sec;
      atime_nsec = e->atime_nsec;
      mtime_sec = e->mtime_sec;
      mtime_nsec = e->mtime_nsec;
      ctime_sec = mtime_sec;
      ctime_nsec = mtime_nsec;
    }
    release(&linux_utime_lock);
  }
  memmove(st + 0, &dev, sizeof(dev));   // st_dev
  memmove(st + 8, &ino, sizeof(ino));   // st_ino
  memmove(st + 16, &mode32, sizeof(mode32)); // st_mode
  memmove(st + 20, &nlink32, sizeof(nlink32)); // st_nlink
  memmove(st + 24, &uid32, sizeof(uid32)); // st_uid
  memmove(st + 28, &gid32, sizeof(gid32)); // st_gid
  memmove(st + 48, &size, sizeof(size)); // st_size
  memmove(st + 56, &blksize, sizeof(blksize)); // st_blksize
  memmove(st + 64, &blocks, sizeof(blocks)); // st_blocks
  memmove(st + 72, &atime_sec, sizeof(atime_sec));
  memmove(st + 80, &atime_nsec, sizeof(atime_nsec));
  memmove(st + 88, &mtime_sec, sizeof(mtime_sec));
  memmove(st + 96, &mtime_nsec, sizeof(mtime_nsec));
  memmove(st + 104, &ctime_sec, sizeof(ctime_sec));
  memmove(st + 112, &ctime_nsec, sizeof(ctime_nsec));
  if(copyout(myproc()->pagetable, staddr, st, sizeof(st)) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_fstat(void)
{
  // Linux fstat; sockets/pipes get synthetic metadata.
  struct file *f;
  uint64 staddr;
  struct vfs_node node;
  uint64 mode = 0100000 | 0777;
  uint64 size = 0;
  uint64 ino = 0;
  uint64 nlink = 1;
  uint64 blocks = 0;
  uint uid = 0;
  uint gid = 0;
  uint64 atime = 0;
  uint64 mtime = 0;
  uint64 ctime = 0;
  struct proc *p = myproc();

  argaddr(1, &staddr);
  if(argfd(0, 0, &f) < 0)
    return -1;

  if(p->is_linux == 0)
    return filestat(f, staddr);

  if(vfs_file_stat_node(f, &node) == 0){
    mode = node.mode | 0777;
    if(mode == 0777){
      if(node.type == T_DIR)
        mode = 0040000 | 0777;
      else if(node.type == T_DEVICE)
        mode = 0020000 | 0777;
      else
        mode = 0100000 | 0777;
    }
    size = node.size;
    ino = node.ino;
    nlink = node.nlink;
    blocks = node.blocks;
    uid = node.uid;
    gid = node.gid;
    atime = node.atime;
    mtime = node.mtime;
    ctime = node.ctime;
  } else {
    mode = 0010000 | 0777;
  }

  return linux_copy_stat_time(staddr, FIRSTDEV, ino, mode, nlink, size,
                              blocks, uid, gid, atime, mtime, ctime, 0, f);
}

uint64
sys_linux_getdents64(void)
{
  // Read Linux dirent64 records from the backing VFS directory.
  struct file *f;
  uint64 dirp;
  int nread;

  argaddr(1, &dirp);
  argint(2, &nread);
  if(argfd(0, 0, &f) < 0 || nread < 48)
    return -1;

  if(f->vfs_ops && f->vfs_ops->file && f->vfs_ops->file->readdir)
    return f->vfs_ops->file->readdir(f, dirp, nread);
  return -1;
}

uint64
sys_linux_mount(void)
{
  // Stub: report success for userland that tries harmless mount probes.
  return 0;
}

uint64
sys_linux_faccessat(void)
{
  // Check whether a path exists; access mode and dirfd are ignored.
  char path[MAXPATH];
  struct vfs_path vp;
  struct vfs_node node;
  struct proc *p = myproc();

  if(argstr(1, path, MAXPATH) < 0)
    return -1;

  if(vfs_resolve_proc_path(p, path, &vp) == 0 &&
     vp.ops && vp.ops->inode && vp.ops->inode->lookup &&
     vp.ops->inode->lookup(vp.mount, vp.inner, &node) == 0)
    return 0;
  return -1;
}

uint64
sys_linux_readlinkat(void)
{
  // Support /proc/self/exe for glibc dynamic linker origin resolution.
  char path[MAXPATH];
  uint64 buf;
  int size;
  char target[MAXPATH];
  int n;

  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  argaddr(2, &buf);
  argint(3, &size);
  if(size <= 0)
    return -1;

  if(strlen(path) == 14 && strncmp(path, "/proc/self/exe", 14) == 0)
    safestrcpy(target, myproc()->linux_exe_path, sizeof(target));
  else
    return -1;

  n = strlen(target);
  if(n > size)
    n = size;
  if(copyout(myproc()->pagetable, buf, target, n) < 0)
    return -1;
  return n;
}

uint64
sys_mkdir(void)
{
  // Create a directory through VFS, with ext4 write redirection.
  char path[MAXPATH];
  char actual[MAXPATH];
  struct vfs_path vp;
  struct proc *p = myproc();

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->mkdir == 0 ||
     vp.ops->inode->mkdir(vp.mount, vp.inner, 0) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_mkdirat(void)
{
  // Linux mkdirat subset; dirfd is ignored.
  char path[MAXPATH], actual[MAXPATH];
  int dirfd, mode;
  struct vfs_path vp;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argint(2, &mode);
  (void)dirfd;
  (void)mode;

  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->mkdir == 0 ||
     vp.ops->inode->mkdir(vp.mount, vp.inner, mode) < 0)
    return -1;
  return 0;
}

uint64
sys_mknod(void)
{
  // Create a device node through VFS.
  char path[MAXPATH], actual[MAXPATH];
  int major, minor;
  struct vfs_path vp;
  struct proc *p = myproc();

  argint(1, &major);
  argint(2, &minor);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->mknod == 0 ||
     vp.ops->inode->mknod(vp.mount, vp.inner, major, minor, 0) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_mknodat(void)
{
  // Linux mknodat subset; dirfd is ignored.
  char path[MAXPATH], actual[MAXPATH];
  int dirfd, major, minor;
  struct vfs_path vp;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argint(2, &major);
  argint(3, &minor);
  (void)dirfd;

  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->mknod == 0 ||
     vp.ops->inode->mknod(vp.mount, vp.inner, major, minor, 0) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_unlinkat(void)
{
  // Linux unlinkat subset; flags and dirfd are ignored.
  char path[MAXPATH], actual[MAXPATH];
  int dirfd, flags;
  struct vfs_path vp;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argint(2, &flags);
  (void)dirfd;
  (void)flags;

  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, path, actual, sizeof(actual), &vp) < 0 ||
     vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->unlink == 0 ||
     vp.ops->inode->unlink(vp.mount, vp.inner) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_linkat(void)
{
  // Linux linkat subset; dirfds and flags are ignored.
  char new[MAXPATH], old[MAXPATH], actual_old[MAXPATH], actual_new[MAXPATH];
  int olddirfd, newdirfd, flags;
  struct vfs_path oldvp, newvp;
  struct proc *p = myproc();

  argint(0, &olddirfd);
  argint(2, &newdirfd);
  argint(4, &flags);
  (void)olddirfd;
  (void)newdirfd;
  (void)flags;

  if(argstr(1, old, MAXPATH) < 0 || argstr(3, new, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, old, actual_old, sizeof(actual_old), &oldvp) < 0 ||
     vfs_prepare_write_path(p, new, actual_new, sizeof(actual_new), &newvp) < 0)
    return -1;
  if(oldvp.mount != newvp.mount ||
     oldvp.ops == 0 || oldvp.ops->inode == 0 || oldvp.ops->inode->link == 0 ||
     oldvp.ops->inode->link(oldvp.mount, oldvp.inner, newvp.inner) < 0)
    return -1;
  return 0;
}

uint64
sys_linux_renameat(void)
{
  // Linux renameat subset; dirfds are ignored.
  char old[MAXPATH], new[MAXPATH], actual_old[MAXPATH], actual_new[MAXPATH];
  int olddirfd, newdirfd;
  struct vfs_path oldvp, newvp;
  struct proc *p = myproc();

  argint(0, &olddirfd);
  argint(2, &newdirfd);
  (void)olddirfd;
  (void)newdirfd;
  if(argstr(1, old, MAXPATH) < 0 || argstr(3, new, MAXPATH) < 0)
    return -1;
  if(vfs_prepare_write_path(p, old, actual_old, sizeof(actual_old), &oldvp) < 0 ||
     vfs_prepare_write_path(p, new, actual_new, sizeof(actual_new), &newvp) < 0)
    return -1;
  if(oldvp.mount != newvp.mount ||
     oldvp.ops == 0 || oldvp.ops->inode == 0 || oldvp.ops->inode->rename == 0 ||
     oldvp.ops->inode->rename(oldvp.mount, oldvp.inner, newvp.inner) < 0)
    return -1;
  return 0;
}

uint64
sys_chdir(void)
{
  // Change VFS cwd and synchronize legacy xv6 cwd when possible.
  char path[MAXPATH];
  struct vfs_path vp;
  struct vfs_node node;
  struct proc *p = myproc();
  
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  if(vfs_resolve_proc_path(p, path, &vp) < 0)
    return -1;
  if(vp.ops == 0 || vp.ops->inode == 0 || vp.ops->inode->lookup == 0)
    return -1;
  if(vp.ops->inode->lookup(vp.mount, vp.inner, &node) < 0 ||
     node.type != T_DIR)
    return -1;

  if(vfs_set_proc_cwd(p, vp.abs_path) < 0)
    return -1;
  if(vfs_sync_legacy_cwd(p, &vp) < 0)
    return -1;
  linux_sync_fs_to_group(p, &vp);
  return 0;
}

uint64
sys_linux_getcwd(void)
{
  // Return cwd relative to the process root, matching Linux chroot behavior.
  uint64 buf;
  int size;
  struct proc *p = myproc();
  char path[MAXPATH];
  int n;

  argaddr(0, &buf);
  argint(1, &size);
  if(size <= 0)
    return -1;

  if(vfs_proc_cwd_path(p, path, sizeof(path)) < 0)
    return -1;
  n = strlen(path) + 1;
  if(n > size)
    return -1;
  if(copyout(p->pagetable, buf, path, n) < 0)
    return -1;
  return buf;
}

uint64
sys_exec(void)
{
  // xv6 exec: copy argv strings from userspace, then replace the image.
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  // Pull the argv vector into kernel pages before kexec destroys old memory.
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_linux_execve(void)
{
  // Linux execve: copy argv and run kexec; envp is currently ignored.
  char path[MAXPATH], binpath[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  memset(argv, 0, sizeof(argv));
  // Copy userspace argv into kernel memory before switching page tables.
  for(i = 0;; i++){
    if(i >= NELEM(argv))
      goto bad;
    if(fetchaddr(uargv + sizeof(uint64) * i, &uarg) < 0)
      goto bad;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  //This is not very graceful, but in this way we can
  //support the linux similar behavior of executing the binary file
  if(ret < 0 && path[0] != '/' && strlen(path) + 6 < sizeof(binpath)){
    int has_slash = 0;
    for(char *s = path; *s; s++){
      if(*s == '/'){
        has_slash = 1;
        break;
      }
    }
    //the binary should be placed in /bin/
    if(has_slash == 0){
      safestrcpy(binpath, "/bin/", sizeof(binpath));
      safestrcpy(binpath + 5, path, sizeof(binpath) - 5);
      ret = kexec(binpath, argv);
    }
  }

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  // Create a pipe and copy the read/write fds back to userspace.
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    if(fd0 >= 0)
      p->ofd_flags[fd0] = 0;
    linux_sync_file_table(p);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    p->ofd_flags[fd0] = 0;
    p->ofd_flags[fd1] = 0;
    linux_sync_file_table(p);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_linux_pipe2(void)
{
  // Create a pipe with Linux pipe2 flags.
  uint64 fdarray;
  struct file *rf, *wf;
  int fd0, fd1;
  int flags;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  argint(1, &flags);

  if(flags & ~(02000000 | 04000))
    return -22; // EINVAL

  if(pipealloc(&rf, &wf) < 0)
    return -24; // EMFILE
  if(flags & 04000){ // O_NONBLOCK
    rf->status_flags |= 04000;
    wf->status_flags |= 04000;
  }

  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    if(fd0 >= 0)
      p->ofd_flags[fd0] = 0;
    linux_sync_file_table(p);
    fileclose(rf);
    fileclose(wf);
    return -24; // EMFILE
  }
  if(flags & 02000000){ // O_CLOEXEC
    p->ofd_flags[fd0] = 1;
    p->ofd_flags[fd1] = 1;
    linux_sync_file_table(p);
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    p->ofd_flags[fd0] = 0;
    p->ofd_flags[fd1] = 0;
    linux_sync_file_table(p);
    fileclose(rf);
    fileclose(wf);
    return -14; // EFAULT
  }
  return 0;
}
