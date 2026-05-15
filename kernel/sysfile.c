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
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

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

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_linux_dup3(void)
{
  int oldfd, newfd, flags;
  struct proc *p = myproc();
  struct file *f;

  argint(0, &oldfd);
  argint(1, &newfd);
  argint(2, &flags);
  (void)flags;

  if(oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE)
    return -1;
  if(oldfd == newfd)
    return -1;
  if((f = p->ofile[oldfd]) == 0)
    return -1;
  if(p->ofile[newfd]){
    fileclose(p->ofile[newfd]);
    p->ofile[newfd] = 0;
  }
  p->ofile[newfd] = filedup(f);
  return newfd;
}

uint64
sys_linux_fcntl(void)
{
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
        return newfd;
      }
    }
    return -1;
  case 1:  // F_GETFD
    return 0;
  case 2:  // F_SETFD
    return 0;
  default:
    return -1;
  }
}

uint64
sys_read(void)
{
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
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  uint64 len;
  int prot, flags, offset;
  struct file *f;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(5, &offset);

  if(addr != 0 || len == 0 || offset != 0)
    return -1;
  if(argfd(4, 0, &f) < 0)
    return -1;
  if((flags != MAP_SHARED && flags != MAP_PRIVATE) || f->type != FD_INODE)
    return -1;
  if((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
    return -1;
  if((prot & PROT_READ) && f->readable == 0)
    return -1;
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && f->writable == 0)
    return -1;

  int slot = -1;
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0){
      slot = i;
      break;
    }
  }
  if(slot < 0)
    return -1;

  uint64 maplen = PGROUNDUP(len);
  uint64 mapaddr = PGROUNDDOWN(p->mmap_base - maplen);
  if(mapaddr < p->sz || mapaddr + maplen > USYSCALL)
    return -1;
  ilock(f->ip);
  uint64 filelen = f->ip->size;
  iunlock(f->ip);

  p->mmap_base = mapaddr;
  p->vmas[slot].used = 1;
  p->vmas[slot].addr = mapaddr;
  p->vmas[slot].len = maplen;
  p->vmas[slot].filelen = filelen;
  p->vmas[slot].prot = prot;
  p->vmas[slot].flags = flags;
  p->vmas[slot].offset = 0;
  p->vmas[slot].f = filedup(f);

  return mapaddr;
}

uint64
sys_linux_mmap(void)
{
  uint64 addr;
  uint64 len;
  int prot, flags, fd, offset;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  if(addr != 0 || len == 0 || offset != 0)
    return -1;
  if((flags & 0x20) == 0) // MAP_ANONYMOUS
    return sys_mmap();

  int slot = -1;
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0){
      slot = i;
      break;
    }
  }
  if(slot < 0)
    return -1;

  uint64 maplen = PGROUNDUP(len);
  uint64 mapaddr = PGROUNDDOWN(p->mmap_base - maplen);
  if(mapaddr < p->sz || mapaddr + maplen > USYSCALL)
    return -1;

  p->mmap_base = mapaddr;
  p->vmas[slot].used = 1;
  p->vmas[slot].addr = mapaddr;
  p->vmas[slot].len = maplen;
  p->vmas[slot].filelen = 0;
  p->vmas[slot].prot = prot;
  p->vmas[slot].flags = flags;
  p->vmas[slot].offset = 0;
  p->vmas[slot].f = 0;
  return mapaddr;
}

uint64
sys_munmap(void)
{
  uint64 addr, len;
  argaddr(0, &addr);
  argaddr(1, &len);

  if(proc_munmap(myproc(), addr, len) < 0)
    return -1;
  return 0;
}

uint64
sys_fstat(void)
{
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
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  if((omode & O_CREATE) == 0){
    char epath[MAXPATH];
    //judge the file wether in ext4 or not
    if(resolve_ext4_path(path, epath, sizeof(epath)) &&
       ext4_path_is_reg(FIRSTDEV, epath)){
      if(omode & (O_WRONLY | O_RDWR))
        return -1;
      if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
        if(f)
          fileclose(f);
        return -1;
      }
      f->type = FD_EXT4;
      f->off = 0;
      f->ip = 0;
      f->readable = !(omode & O_WRONLY);
      f->writable = 0;
      safestrcpy(f->ext4_path, epath, sizeof(f->ext4_path));
      return fd;
    }
  }

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

static int
linux_open_flags(int flags)
{
  int omode = flags & 3;
  if(flags & 0x40)   // O_CREAT
    omode |= O_CREATE;
  if(flags & 0x200)  // O_TRUNC
    omode |= O_TRUNC;
  return omode;
}

uint64
sys_linux_openat(void)
{
  char path[MAXPATH];
  int fd, dirfd, flags, omode;
  uint64 mode;
  struct file *f;
  struct inode *ip;

  argint(0, &dirfd);
  argint(2, &flags);
  argaddr(3, &mode);
  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  (void)dirfd;

  omode = linux_open_flags(flags);
  if((omode & O_CREATE) == 0){
    char epath[MAXPATH];
    if(resolve_ext4_path(path, epath, sizeof(epath)) &&
       ext4_path_is_reg(FIRSTDEV, epath)){
      if(omode & (O_WRONLY | O_RDWR))
        return -1;
      if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
        if(f)
          fileclose(f);
        return -1;
      }
      f->type = FD_EXT4;
      f->off = 0;
      f->ip = 0;
      f->readable = 1;
      f->writable = 0;
      safestrcpy(f->ext4_path, epath, sizeof(f->ext4_path));
      return fd;
    }
  }

  begin_op();
  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  if((omode & O_TRUNC) && ip->type == T_FILE)
    itrunc(ip);
  iunlock(ip);
  end_op();
  return fd;
}

uint64
sys_linux_writev(void)
{
  struct file *f;
  int fd, iovcnt;
  uint64 iov;
  uint64 base, len;
  int total = 0;

  argint(0, &fd);
  argaddr(1, &iov);
  argint(2, &iovcnt);
  if(argfd(0, 0, &f) < 0 || iovcnt < 0 || iovcnt > 16)
    return -1;

  for(int i = 0; i < iovcnt; i++){
    if(fetchaddr(iov + i * 16, &base) < 0 ||
       fetchaddr(iov + i * 16 + 8, &len) < 0)
      return -1;
    if(len > 0x7fffffff)
      return -1;
    int n = filewrite(f, base, len);
    if(n < 0)
      return total > 0 ? total : -1;
    total += n;
  }
  return total;
}

uint64
sys_linux_sendfile(void)
{
  int outfd, infd;
  uint64 offaddr, count;
  uint64 off;
  uint64 done = 0;
  struct proc *p = myproc();
  struct file *outf, *inf;
  char *buf;

  argint(0, &outfd);
  argint(1, &infd);
  argaddr(2, &offaddr);
  argaddr(3, &count);

  if(outfd < 0 || outfd >= NOFILE || infd < 0 || infd >= NOFILE)
    return -1;
  if((outf = p->ofile[outfd]) == 0 || (inf = p->ofile[infd]) == 0)
    return -1;
  if(outf->type != FD_PIPE || outf->writable == 0)
    return -1;
  if(inf->type != FD_EXT4 && inf->type != FD_INODE)
    return -1;
  if(offaddr != 0){
    if(fetchaddr(offaddr, &off) < 0)
      return -1;
  } else {
    off = inf->off;
  }

  buf = kalloc();
  if(buf == 0)
    return -1;

  while(done < count){
    int n = count - done;
    int r, w;

    if(n > PGSIZE)
      n = PGSIZE;
    if(inf->type == FD_EXT4){
      r = ext4_read_file_by_path_at(FIRSTDEV, inf->ext4_path, (uchar *)buf, n, off);
    } else {
      ilock(inf->ip);
      r = readi(inf->ip, 0, (uint64)buf, off, n);
      iunlock(inf->ip);
    }
    if(r <= 0)
      break;

    w = pipewrite_kernel(outf->pipe, buf, r);
    if(w < 0){
      if(done == 0)
        done = -1;
      break;
    }
    done += w;
    off += w;
    if(w < r)
      break;
  }

  kfree(buf);
  if(done != (uint64)-1 && done > 0){
    if(offaddr != 0)
      copyout(p->pagetable, offaddr, (char *)&off, sizeof(off));
    else
      inf->off = off;
  }
  return done;
}

uint64
sys_linux_ppoll(void)
{
  uint64 fds;
  int nfds;
  int ready = 0;
  struct proc *p = myproc();

  argaddr(0, &fds);
  argint(1, &nfds);
  if(nfds < 0 || nfds > 64)
    return -1;

  for(int i = 0; i < nfds; i++){
    uint64 addr = fds + i * 8;
    int fd;
    short events;
    short revents = 0;

    if(copyin(p->pagetable, (char *)&fd, addr, sizeof(fd)) < 0 ||
       copyin(p->pagetable, (char *)&events, addr + 4, sizeof(events)) < 0)
      return -1;
    if(fd >= 0 && fd < NOFILE && p->ofile[fd]){
      revents = events & 0x5; // POLLIN | POLLOUT
      if(revents == 0)
        revents = 0x1;
      ready++;
    }
    if(copyout(p->pagetable, addr + 6, (char *)&revents, sizeof(revents)) < 0)
      return -1;
  }
  return ready;
}

uint64
sys_linux_newfstatat(void)
{
  char path[MAXPATH];
  uint64 staddr;
  int dirfd, flags;

  argint(0, &dirfd);
  argaddr(2, &staddr);
  argint(3, &flags);
  (void)flags;
  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  if(path[0] == 0)
    return -1;
  (void)dirfd;

  char st[128];
  memset(st, 0, sizeof(st));
  uint64 mode = 0100000 | 0777;
  uint64 size = 0;
  char epath[MAXPATH];
  if(resolve_ext4_path(path, epath, sizeof(epath)) &&
     ext4_path_is_reg(FIRSTDEV, epath)){
    size = ext4_file_size_by_path(FIRSTDEV, epath);
  } else if(resolve_ext4_path(path, epath, sizeof(epath)) &&
            ext4_path_is_dir(FIRSTDEV, epath)){
    mode = 0040000 | 0777;
  } else {
    struct inode *ip;
    begin_op();
    ip = namei(path);
    if(ip == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR)
      mode = 0040000 | 0777;
    else
      size = ip->size;
    iunlockput(ip);
    end_op();
  }

  // Minimal riscv64 Linux struct stat fields used by libc/busybox.
  memmove(st + 16, &mode, sizeof(mode)); // st_mode
  memmove(st + 48, &size, sizeof(size)); // st_size
  if(copyout(myproc()->pagetable, staddr, st, sizeof(st)) < 0)
    return -1;
  return 0;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_linux_mkdirat(void)
{
  char path[MAXPATH];
  int dirfd, mode;
  struct inode *ip;

  argint(0, &dirfd);
  argint(2, &mode);
  (void)dirfd;
  (void)mode;

  begin_op();
  if(argstr(1, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_linux_mknodat(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int dirfd, major, minor;

  argint(0, &dirfd);
  argint(2, &major);
  argint(3, &minor);
  (void)dirfd;

  begin_op();
  if((argstr(1, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_linux_unlinkat(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;
  int dirfd, flags;

  argint(0, &dirfd);
  argint(2, &flags);
  (void)dirfd;
  (void)flags;

  if(argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

uint64
sys_linux_linkat(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;
  int olddirfd, newdirfd, flags;

  argint(0, &olddirfd);
  argint(2, &newdirfd);
  argint(4, &flags);
  (void)olddirfd;
  (void)newdirfd;
  (void)flags;

  if(argstr(1, old, MAXPATH) < 0 || argstr(3, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  char epath[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(path)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      end_op();
      return -1;
    }
    iunlock(ip);
    iput(p->cwd);
    end_op();
    p->cwd = ip;
    p->cwd_is_ext4 = 0;
    safestrcpy(p->ext4_cwd, "/", sizeof(p->ext4_cwd));
    return 0;
  }
  end_op();

  if(resolve_ext4_path(path, epath, sizeof(epath)) &&
     ext4_path_is_dir(FIRSTDEV, epath)){
    p->cwd_is_ext4 = 1;
    safestrcpy(p->ext4_cwd, epath, sizeof(p->ext4_cwd));
    return 0;
  }

  return -1;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
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
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  memset(argv, 0, sizeof(argv));
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
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_linux_pipe2(void)
{
  return sys_pipe();
}
