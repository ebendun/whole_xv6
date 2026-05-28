#include "types.h"
#include "param.h"
#include "riscv.h"
#include "stat.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "proc.h"
#include "file.h"
#include "fcntl.h"
#include "vfs.h"
#include "lwext4_xv6.h"

static struct spinlock vfs_lock;
static struct vfs_mount mounts[VFS_MAX_MOUNTS];

#define VFS_MOUNT_READY 1
#define VFS_MOUNT_BUSY  2

static struct vfs_block_mount mount_data[VFS_MAX_MOUNTS];

static int xv6_vfs_lookup(struct vfs_mount*, char*, struct vfs_node*);
static int xv6_vfs_create(struct vfs_mount*, char*, int, int, struct vfs_node*);
static int xv6_vfs_mknod(struct vfs_mount*, char*, int, int, int);
static int xv6_vfs_mkdir(struct vfs_mount*, char*, int);
static int xv6_vfs_unlink(struct vfs_mount*, char*);
static int xv6_vfs_link(struct vfs_mount*, char*, char*);
static int xv6_vfs_rename(struct vfs_mount*, char*, char*);
static int xv6_vfs_open(struct vfs_mount*, char*, int, struct file**);
static int xv6_vfs_read(struct file*, uint64, int);
static int xv6_vfs_write(struct file*, uint64, int);
static int xv6_vfs_readdir(struct file*, uint64, int);
static uint64 xv6_vfs_lseek(struct file*, uint64, int);
static int xv6_vfs_stat(struct file*, uint64);
static int ext4_vfs_lookup(struct vfs_mount*, char*, struct vfs_node*);
static int ext4_vfs_create(struct vfs_mount*, char*, int, int, struct vfs_node*);
static int ext4_vfs_mkdir(struct vfs_mount*, char*, int);
static int ext4_vfs_unlink(struct vfs_mount*, char*);
static int ext4_vfs_link(struct vfs_mount*, char*, char*);
static int ext4_vfs_rename(struct vfs_mount*, char*, char*);
static int ext4_vfs_symlink(struct vfs_mount*, char*, char*);
static int ext4_vfs_open(struct vfs_mount*, char*, int, struct file**);
static int ext4_vfs_close(struct file*);
static int ext4_vfs_read(struct file*, uint64, int);
static int ext4_vfs_write(struct file*, uint64, int);
static int ext4_vfs_readdir(struct file*, uint64, int);
static uint64 ext4_vfs_lseek(struct file*, uint64, int);
static int ext4_vfs_stat(struct file*, uint64);

static struct vfs_inode_ops xv6_inode_ops = {
  .lookup = xv6_vfs_lookup,
  .create = xv6_vfs_create,
  .mknod = xv6_vfs_mknod,
  .mkdir = xv6_vfs_mkdir,
  .rmdir = xv6_vfs_unlink,
  .unlink = xv6_vfs_unlink,
  .link = xv6_vfs_link,
  .rename = xv6_vfs_rename,
};

static struct vfs_file_ops xv6_file_ops = {
  .open = xv6_vfs_open,
  .read = xv6_vfs_read,
  .write = xv6_vfs_write,
  .readdir = xv6_vfs_readdir,
  .lseek = xv6_vfs_lseek,
  .stat = xv6_vfs_stat,
};

static struct vfs_ops xv6_ops = {
  .name = "xv6",
  .inode = &xv6_inode_ops,
  .file = &xv6_file_ops,
};

static struct vfs_inode_ops ext4_inode_ops = {
  .lookup = ext4_vfs_lookup,
  .create = ext4_vfs_create,
  .mkdir = ext4_vfs_mkdir,
  .rmdir = ext4_vfs_unlink,
  .unlink = ext4_vfs_unlink,
  .link = ext4_vfs_link,
  .rename = ext4_vfs_rename,
  .symlink = ext4_vfs_symlink,
};

static struct vfs_file_ops ext4_file_ops = {
  .open = ext4_vfs_open,
  .close = ext4_vfs_close,
  .read = ext4_vfs_read,
  .write = ext4_vfs_write,
  .readdir = ext4_vfs_readdir,
  .lseek = ext4_vfs_lseek,
  .stat = ext4_vfs_stat,
};

static struct vfs_ops ext4_ops = {
  .name = "ext4",
  .inode = &ext4_inode_ops,
  .file = &ext4_file_ops,
};

static struct vfs_block_mount*
vfs_block_data(struct vfs_mount *mnt)
{
  return (struct vfs_block_mount*)mnt->private;
}

static int
vfs_dirent64_reclen(char *name)
{
  int n = strlen(name) + 1;
  return (19 + n + 7) & ~7;
}

static int
vfs_copy_dirent64(uint64 dst, uint64 ino, uint64 off, uchar type, char *name)
{
  char de[280];
  ushort reclen;
  int namelen = strlen(name);

  reclen = (ushort)vfs_dirent64_reclen(name);
  if(reclen > sizeof(de))
    return -1;
  memset(de, 0, reclen);
  memmove(de, &ino, sizeof(ino));
  memmove(de + 8, &off, sizeof(off));
  memmove(de + 16, &reclen, sizeof(reclen));
  de[18] = type;
  memmove(de + 19, name, namelen + 1);
  if(copyout(myproc()->pagetable, dst, de, reclen) < 0)
    return -1;
  return reclen;
}

static char*
vfs_root_mount_child(struct vfs_mount *mnt)
{
  char *name;

  if(mnt == 0 || mnt->used != VFS_MOUNT_READY)
    return 0;
  if(mnt->target[0] != '/' || mnt->target[1] == 0)
    return 0;

  name = mnt->target + 1;
  for(char *p = name; *p; p++){
    if(*p == '/')
      return 0;
  }
  return name;
}

static int
vfs_linux_passthrough_abs(char *path)
{
  if(path == 0 || path[0] != '/')
    return 0;
  if((strncmp(path, "/dev", 4) == 0 && (path[4] == 0 || path[4] == '/')) ||
     (strncmp(path, "/proc", 5) == 0 && (path[5] == 0 || path[5] == '/')) ||
     (strncmp(path, "/tmp", 4) == 0 && (path[4] == 0 || path[4] == '/')) ||
     (strncmp(path, "/bin", 4) == 0 && (path[4] == 0 || path[4] == '/')))
    return 1;
  return 0;
}

static int
vfs_copy_root_mounts(uint64 dirp, int nread, int n, uint64 dirsize, uint64 *offp)
{
  int seen = 0;
  int start;

  if(offp == 0 || *offp < dirsize)
    return n;

  start = (*offp - dirsize) / sizeof(struct dirent);
  for(int i = 0; i < VFS_MAX_MOUNTS; i++){
    char *name = vfs_root_mount_child(&mounts[i]);
    int r;
    uint64 nextoff;

    if(name == 0)
      continue;
    if(seen++ < start)
      continue;

    r = vfs_dirent64_reclen(name);
    if(r < 0 || n + r > nread)
      break;

    nextoff = dirsize + seen * sizeof(struct dirent);
    r = vfs_copy_dirent64(dirp + n, ROOTINO + 10000 + seen, nextoff, 4, name);
    if(r < 0)
      break;
    n += r;
    *offp = nextoff;
  }

  return n;
}

static uchar
vfs_linux_dtype_from_ext4(uchar type)
{
  if(type == 2)
    return 4; // DT_DIR
  if(type == 1)
    return 8; // DT_REG
  return 0;
}

static int
xv6_vfs_fill_node(struct vfs_mount *mnt, struct inode *ip, struct vfs_node *out)
{
  if(out == 0)
    return 0;

  memset(out, 0, sizeof(*out));
  out->type = ip->type;
  out->ino = ip->inum;
  if(ip->type == T_DIR)
    out->mode = 0040000 | 0777;
  else if(ip->type == T_DEVICE)
    out->mode = 0020000 | 0777;
  else
    out->mode = 0100000 | 0777;
  out->size = ip->size;
  out->nlink = ip->nlink;
  out->blocks = (ip->size + 511) / 512;
  out->uid = 0;
  out->gid = 0;
  out->mount = mnt;
  return 0;
}

static int
xv6_vfs_isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off = 2 * sizeof(de); off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("vfs isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

static struct inode*
xv6_vfs_create_inode(char *path, short type, short major, short minor)
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

  if(type == T_DIR){
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    dp->nlink++;
    iupdate(dp);
  }

  iunlockput(dp);
  return ip;

fail:
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

static int
xv6_vfs_lookup(struct vfs_mount *mnt, char *path, struct vfs_node *out)
{
  struct inode *ip;
  (void)mnt;

  if(path == 0 || out == 0)
    return -1;

  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  xv6_vfs_fill_node(mnt, ip, out);
  iunlock(ip);
  iput(ip);
  end_op();
  return 0;
}

static int
xv6_vfs_create(struct vfs_mount *mnt, char *path, int type, int mode, struct vfs_node *out)
{
  struct inode *ip;
  (void)mode;

  if(path == 0)
    return -1;

  begin_op();
  ip = xv6_vfs_create_inode(path, type, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }
  xv6_vfs_fill_node(mnt, ip, out);
  iunlockput(ip);
  end_op();
  return 0;
}

static int
xv6_vfs_mknod(struct vfs_mount *mnt, char *path, int major, int minor, int mode)
{
  struct inode *ip;
  (void)mode;

  if(path == 0)
    return -1;

  begin_op();
  ip = xv6_vfs_create_inode(path, T_DEVICE, major, minor);
  if(ip == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  (void)mnt;
  return 0;
}

static int
xv6_vfs_mkdir(struct vfs_mount *mnt, char *path, int mode)
{
  struct inode *ip;
  (void)mode;

  if(path == 0)
    return -1;

  begin_op();
  if((ip = namei(path)) != 0){
    ilock(ip);
    if(ip->type == T_DIR){
      iunlockput(ip);
      end_op();
      return 0;
    }
    iunlockput(ip);
    end_op();
    return -1;
  }
  ip = xv6_vfs_create_inode(path, T_DIR, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  (void)mnt;
  return 0;
}

static int
xv6_vfs_unlink(struct vfs_mount *mnt, char *path)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ];
  uint off;
  (void)mnt;

  if(path == 0)
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
    panic("vfs unlink: nlink < 1");
  if(ip->type == T_DIR && !xv6_vfs_isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("vfs unlink: writei");
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

static int
xv6_vfs_link(struct vfs_mount *mnt, char *old, char *new)
{
  char name[DIRSIZ];
  struct inode *dp, *ip;
  (void)mnt;

  if(old == 0 || new == 0)
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

static int
xv6_vfs_rename(struct vfs_mount *mnt, char *old, char *new)
{
  char oldname[DIRSIZ], newname[DIRSIZ];
  struct inode *olddp, *newdp, *ip, *exist;
  struct dirent de;
  uint off;
  (void)mnt;

  if(old == 0 || new == 0)
    return -1;

  begin_op();
  if((olddp = nameiparent(old, oldname)) == 0){
    end_op();
    return -1;
  }
  if((newdp = nameiparent(new, newname)) == 0){
    iput(olddp);
    end_op();
    return -1;
  }

  if(olddp->dev != newdp->dev || olddp->inum != newdp->inum ||
     strlen(newname) >= DIRSIZ){
    iput(newdp);
    iput(olddp);
    end_op();
    return -1;
  }
  iput(newdp);

  ilock(olddp);
  if((ip = dirlookup(olddp, oldname, &off)) == 0)
    goto bad;

  if((exist = dirlookup(olddp, newname, 0)) != 0){
    ilock(exist);
    if(exist->type == T_DIR && !xv6_vfs_isdirempty(exist)){
      iunlockput(exist);
      iput(ip);
      goto bad;
    }
    iunlockput(exist);
  }

  memset(&de, 0, sizeof(de));
  de.inum = ip->inum;
  strncpy(de.name, newname, DIRSIZ);
  if(writei(olddp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("vfs rename: writei");
  iput(ip);
  iunlockput(olddp);
  end_op();
  return 0;

bad:
  iunlockput(olddp);
  end_op();
  return -1;
}

static int
xv6_vfs_open(struct vfs_mount *mnt, char *path, int flags, struct file **fp)
{
  struct inode *ip;
  struct file *f;
  int omode = flags;
  (void)mnt;

  if(path == 0 || fp == 0)
    return -1;
  *fp = 0;

  if(omode & O_CREATE)
    return -1;

  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR && omode != O_RDONLY){
    iunlockput(ip);
    end_op();
    return -1;
  }
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  f = filealloc();
  if(f == 0){
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
  f->status_flags = omode & O_APPEND;
  f->vfs_ops = mnt->ops;

  if((omode & O_TRUNC) && ip->type == T_FILE)
    itrunc(ip);

  iunlock(ip);
  end_op();
  *fp = f;
  return 0;
}

static int
xv6_vfs_readdir(struct file *f, uint64 dirp, int nread)
{
  struct dirent de;
  int n = 0;
  int r;
  int isroot;
  uint dirsize;

  if(f == 0 || f->type != FD_INODE || nread < 48)
    return -1;

  ilock(f->ip);
  if(f->ip->type != T_DIR){
    iunlock(f->ip);
    return -1;
  }
  isroot = f->ip->inum == ROOTINO;
  dirsize = f->ip->size;
  while(f->off + sizeof(de) <= f->ip->size){
    if(readi(f->ip, 0, (uint64)&de, f->off, sizeof(de)) != sizeof(de))
      break;
    f->off += sizeof(de);
    if(de.inum == 0)
      continue;
    de.name[DIRSIZ - 1] = 0;
    r = vfs_dirent64_reclen(de.name);
    if(r < 0 || n + r > nread)
      break;
    r = vfs_copy_dirent64(dirp + n, de.inum, f->off, 0, de.name);
    if(r < 0)
      break;
    n += r;
  }
  iunlock(f->ip);
  if(isroot && n < nread)
    n = vfs_copy_root_mounts(dirp, nread, n, dirsize, &f->off);
  return n;
}

static int
xv6_vfs_read(struct file *f, uint64 addr, int n)
{
  int r;

  if(f == 0 || f->type != FD_INODE)
    return -1;
  ilock(f->ip);
  if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
    f->off += r;
  iunlock(f->ip);
  return r;
}

static int
xv6_vfs_write(struct file *f, uint64 addr, int n)
{
  int r;
  int i = 0;
  int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;

  if(f == 0 || f->type != FD_INODE)
    return -1;

  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    begin_op();
    ilock(f->ip);
    if(f->status_flags & O_APPEND)
      f->off = f->ip->size;
    if((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
      f->off += r;
    iunlock(f->ip);
    end_op();

    if(r != n1)
      break;
    i += r;
  }
  return i == n ? n : -1;
}

static uint64
xv6_vfs_lseek(struct file *f, uint64 off, int whence)
{
  uint64 size;

  if(f == 0 || f->type != FD_INODE)
    return -1;

  ilock(f->ip);
  size = f->ip->size;
  iunlock(f->ip);

  if(whence == 0)
    f->off = off;
  else if(whence == 1)
    f->off += off;
  else if(whence == 2)
    f->off = size + off;
  else
    return -1;

  return f->off;
}

static int
xv6_vfs_stat(struct file *f, uint64 addr)
{
  struct stat st;

  if(f == 0 || (f->type != FD_INODE && f->type != FD_DEVICE))
    return -1;

  ilock(f->ip);
  stati(f->ip, &st);
  iunlock(f->ip);
  if(copyout(myproc()->pagetable, addr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return 0;
}

static int
ext4_vfs_lookup(struct vfs_mount *mnt, char *path, struct vfs_node *out)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);
  struct lwext4_xv6_stat st;

  if(data == 0 || path == 0 || out == 0)
    return -1;
  if(lwext4_xv6_stat(data->dev, path, &st) < 0)
    return -1;

  memset(out, 0, sizeof(*out));
  out->type = st.type;
  out->ino = st.ino;
  out->mode = st.mode;
  out->size = st.size;
  out->nlink = st.nlink;
  out->blocks = st.blocks;
  out->uid = st.uid;
  out->gid = st.gid;
  out->atime = st.atime;
  out->mtime = st.mtime;
  out->ctime = st.ctime;
  out->mount = mnt;
  return 0;
}

static int
ext4_vfs_create(struct vfs_mount *mnt, char *path, int type, int mode,
                struct vfs_node *out)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);
  (void)mode;

  if(data == 0 || path == 0 || type != T_FILE)
    return -1;
  if(lwext4_xv6_create_file(data->dev, path) < 0)
    return -1;
  if(out)
    return ext4_vfs_lookup(mnt, path, out);
  return 0;
}

static int
ext4_vfs_mkdir(struct vfs_mount *mnt, char *path, int mode)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);
  (void)mode;

  if(data == 0 || path == 0)
    return -1;
  return lwext4_xv6_mkdir(data->dev, path);
}

static int
ext4_vfs_unlink(struct vfs_mount *mnt, char *path)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);

  if(data == 0 || path == 0)
    return -1;
  return lwext4_xv6_unlink(data->dev, path);
}

static int
ext4_vfs_link(struct vfs_mount *mnt, char *old, char *new)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);

  if(data == 0 || old == 0 || new == 0)
    return -1;
  return lwext4_xv6_link(data->dev, old, new);
}

static int
ext4_vfs_rename(struct vfs_mount *mnt, char *old, char *new)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);

  if(data == 0 || old == 0 || new == 0)
    return -1;
  return lwext4_xv6_rename(data->dev, old, new);
}

static int
ext4_vfs_symlink(struct vfs_mount *mnt, char *target, char *path)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);

  if(data == 0 || target == 0 || path == 0)
    return -1;
  return lwext4_xv6_symlink(data->dev, target, path);
}

static int
ext4_vfs_open(struct vfs_mount *mnt, char *path, int flags, struct file **fp)
{
  struct vfs_block_mount *data = vfs_block_data(mnt);
  struct file *f;
  int omode = flags;
  void *handle = 0;
  int type = 0;

  if(data == 0 || path == 0 || fp == 0)
    return -1;
  *fp = 0;

  if(lwext4_xv6_open(data->dev, path, omode, &handle, &type, 0, 0) < 0)
    return -1;
  if(type == T_DIR && (omode & 3) != O_RDONLY){
    lwext4_xv6_close_dir(handle);
    return -1;
  }

  f = filealloc();
  if(f == 0){
    if(type == T_DIR)
      lwext4_xv6_close_dir(handle);
    else
      lwext4_xv6_close(handle);
    return -1;
  }

  f->type = FD_EXT4;
  f->off = 0;
  f->ip = 0;
  f->fs_file = handle;
  f->fs_is_dir = type == T_DIR;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  f->status_flags = omode & O_APPEND;
  f->vfs_ops = mnt->ops;
  safestrcpy(f->ext4_path, path, sizeof(f->ext4_path));
  *fp = f;
  return 0;
}

static int
ext4_vfs_close(struct file *f)
{
  if(f == 0 || f->type != FD_EXT4)
    return -1;
  if(f->fs_file == 0)
    return 0;
  if(f->fs_is_dir){
    if(lwext4_xv6_close_dir(f->fs_file) < 0)
      return -1;
  } else {
    if(lwext4_xv6_close(f->fs_file) < 0)
      return -1;
  }
  f->fs_file = 0;
  return 0;
}

static int
ext4_vfs_readdir(struct file *f, uint64 dirp, int nread)
{
  int n = 0;
  int r;

  if(f == 0 || f->type != FD_EXT4 || nread < 48)
    return -1;
  if(f->fs_is_dir == 0 || f->fs_file == 0)
    return -1;

  for(;;){
    uint64 next, ino;
    uchar type;
    char name[256];
    if(nread - n < 280)
      break;
    int ok = lwext4_xv6_dirent_next(f->fs_file, &next, &ino, &type, name,
                                    sizeof(name));
    if(ok < 0)
      return n > 0 ? n : -1;
    if(ok == 0)
      break;
    r = vfs_dirent64_reclen(name);
    if(r < 0 || n + r > nread)
      break;
    r = vfs_copy_dirent64(dirp + n, ino, next, vfs_linux_dtype_from_ext4(type), name);
    if(r < 0)
      break;
    n += r;
    f->off = next;
  }
  return n;
}

static int
ext4_vfs_read(struct file *f, uint64 addr, int n)
{
  char *buf;
  int m;
  int r;

  if(f == 0 || f->type != FD_EXT4)
    return -1;
  if(f->fs_is_dir){
    struct dirent de;
    uint64 next, ino;
    uchar type;
    char name[DIRSIZ + 1];
    int ok;

    if(n < sizeof(de))
      return -1;
    for(;;){
      ok = lwext4_xv6_dirent_next(f->fs_file, &next, &ino, &type, name,
                                  sizeof(name));
      if(ok <= 0)
        return ok;
      f->off = next;
      memset(&de, 0, sizeof(de));
      de.inum = ino;
      safestrcpy(de.name, name, sizeof(de.name));
      if(copyout(myproc()->pagetable, addr, (char *)&de, sizeof(de)) < 0)
        return -1;
      return sizeof(de);
    }
  }

  buf = kalloc();
  if(buf == 0)
    return -1;
  m = n;
  if(m > PGSIZE)
    m = PGSIZE;
  if(f->fs_file)
    r = lwext4_xv6_read(f->fs_file, (uchar *)buf, m, f->off);
  else
    r = lwext4_xv6_read_file_by_path_at(FIRSTDEV, f->ext4_path, (uchar *)buf, m, f->off);
  if(r > 0){
    if(copyout(myproc()->pagetable, addr, buf, r) < 0)
      r = -1;
    else
      f->off += r;
  }
  kfree(buf);
  return r;
}

static int
ext4_vfs_write(struct file *f, uint64 addr, int n)
{
  char *buf;
  int done = 0;

  if(f == 0 || f->type != FD_EXT4 || n < 0)
    return -1;
  if(lwext4_xv6_path_is_dir(FIRSTDEV, f->ext4_path))
    return -1;

  buf = kalloc();
  if(buf == 0)
    return -1;
  while(done < n){
    int m = n - done;
    int r;

    if(m > PGSIZE)
      m = PGSIZE;
    if(f->status_flags & O_APPEND){
      if(f->fs_file)
        f->off = lwext4_xv6_file_size(f->fs_file);
      else
        f->off = lwext4_xv6_file_size_by_path(FIRSTDEV, f->ext4_path);
    }
    if(copyin(myproc()->pagetable, buf, addr + done, m) < 0){
      kfree(buf);
      return done > 0 ? done : -1;
    }
    if(f->fs_file)
      r = lwext4_xv6_write(f->fs_file, (uchar *)buf, m, f->off);
    else
      r = lwext4_xv6_write_file_by_path_at(FIRSTDEV, f->ext4_path,
                                           (uchar *)buf, m, f->off);
    if(r <= 0){
      kfree(buf);
      return done > 0 ? done : -1;
    }
    f->off += r;
    done += r;
    if(r != m)
      break;
  }
  kfree(buf);
  return done;
}

static uint64
ext4_vfs_lseek(struct file *f, uint64 off, int whence)
{
  uint64 size;

  if(f == 0 || f->type != FD_EXT4)
    return -1;

  if(f->fs_file)
    size = lwext4_xv6_file_size(f->fs_file);
  else
    size = lwext4_xv6_file_size_by_path(FIRSTDEV, f->ext4_path);
  if(whence == 0)
    f->off = off;
  else if(whence == 1)
    f->off += off;
  else if(whence == 2)
    f->off = size + off;
  else
    return -1;

  return f->off;
}

static int
ext4_vfs_stat(struct file *f, uint64 addr)
{
  struct stat st;
  struct lwext4_xv6_stat est;

  if(f == 0 || f->type != FD_EXT4)
    return -1;
  if(lwext4_xv6_stat(FIRSTDEV, f->ext4_path, &est) < 0)
    return -1;

  memset(&st, 0, sizeof(st));
  st.dev = FIRSTDEV;
  st.ino = est.ino;
  st.type = est.type;
  st.nlink = est.nlink;
  st.size = est.size;
  if(copyout(myproc()->pagetable, addr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return 0;
}

int
vfs_file_stat_node(struct file *f, struct vfs_node *node)
{
  if(f == 0 || node == 0)
    return -1;

  memset(node, 0, sizeof(*node));
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    xv6_vfs_fill_node(0, f->ip, node);
    iunlock(f->ip);
    return 0;
  }

  if(f->type == FD_EXT4){
    struct lwext4_xv6_stat st;

    if(lwext4_xv6_stat(FIRSTDEV, f->ext4_path, &st) < 0)
      return -1;
    node->type = st.type;
    node->ino = st.ino;
    node->mode = st.mode;
    node->size = st.size;
    node->nlink = st.nlink;
    node->blocks = st.blocks;
    node->uid = st.uid;
    node->gid = st.gid;
    node->atime = st.atime;
    node->mtime = st.mtime;
    node->ctime = st.ctime;
    return 0;
  }

  return -1;
}

int
vfs_file_read_kernel(struct file *f, char *buf, int n, uint64 off)
{
  int r;

  if(f == 0 || buf == 0 || n < 0)
    return -1;

  if(f->type == FD_INODE){
    ilock(f->ip);
    r = readi(f->ip, 0, (uint64)buf, off, n);
    iunlock(f->ip);
    return r;
  }

  if(f->type == FD_EXT4){
    if(f->fs_file)
      return lwext4_xv6_read(f->fs_file, (uchar *)buf, n, off);
    return lwext4_xv6_read_file_by_path_at(FIRSTDEV, f->ext4_path, (uchar *)buf, n, off);
  }

  return -1;
}

int
vfs_file_write_kernel(struct file *f, char *buf, int n, uint64 off)
{
  int r;

  if(f == 0 || buf == 0 || n < 0)
    return -1;

  if(f->type == FD_INODE){
    begin_op();
    ilock(f->ip);
    r = writei(f->ip, 0, (uint64)buf, off, n);
    iunlock(f->ip);
    end_op();
    return r;
  }

  if(f->type == FD_EXT4){
    if(f->fs_file)
      return lwext4_xv6_write(f->fs_file, (uchar *)buf, n, off);
    return lwext4_xv6_write_file_by_path_at(FIRSTDEV, f->ext4_path,
                                            (uchar *)buf, n, off);
  }

  return -1;
}

static int
vfs_streq(const char *a, const char *b)
{
  int n = strlen(a);
  return strlen(b) == n && strncmp(a, b, n) == 0;
}

static void
vfs_clean_path(char *dst, int dstsz, const char *src)
{
  int di = 0;
  int absolute;

  if(dstsz <= 0)
    return;
  if(src == 0 || src[0] == 0){
    safestrcpy(dst, "/", dstsz);
    return;
  }

  absolute = src[0] == '/';
  if(absolute)
    dst[di++] = '/';

  for(int si = 0; src[si] && di < dstsz - 1; ){
    int start, len;

    while(src[si] == '/')
      si++;
    start = si;
    while(src[si] && src[si] != '/')
      si++;
    len = si - start;
    if(len == 0)
      break;

    if(len == 1 && src[start] == '.')
      continue;
    if(len == 2 && src[start] == '.' && src[start + 1] == '.'){
      int root = absolute ? 1 : 0;

      if(di > root){
        if(di > root && dst[di - 1] == '/')
          di--;
        while(di > root && dst[di - 1] != '/')
          di--;
      }
      continue;
    }

    if(di > 0 && dst[di - 1] != '/' && di < dstsz - 1)
      dst[di++] = '/';
    for(int i = 0; i < len && di < dstsz - 1; i++)
      dst[di++] = src[start + i];
  }

  if(di == 0)
    dst[di++] = '/';
  while(di > 1 && dst[di - 1] == '/')
    di--;
  dst[di] = 0;
}

static void
vfs_normalize_target(char *dst, int dstsz, const char *src)
{
  vfs_clean_path(dst, dstsz, src);
}

static int
vfs_type_from_name(const char *fstype)
{
  if(fstype == 0 || fstype[0] == 0)
    return VFS_NONE;
  if(vfs_streq(fstype, "xv6"))
    return VFS_XV6;
  if(vfs_streq(fstype, "ext4"))
    return VFS_EXT4;
  return VFS_NONE;
}

static struct vfs_ops*
vfs_ops_for_type(int type)
{
  switch(type){
  case VFS_XV6:
    return &xv6_ops;
  case VFS_EXT4:
    return &ext4_ops;
  default:
    return 0;
  }
}

static void*
vfs_private_for_mount(int slot, int type, char *source)
{
  memset(&mount_data[slot], 0, sizeof(mount_data[slot]));
  (void)source;

  if(type == VFS_XV6)
    mount_data[slot].dev = SECONDDEV;
  else if(type == VFS_EXT4)
    mount_data[slot].dev = FIRSTDEV;
  else
    return 0;
  return &mount_data[slot];
}

static int
vfs_path_matches_mount(const char *path, const char *target)
{
  int n;

  if(vfs_streq(target, "/"))
    return path[0] == '/';

  n = strlen(target);
  return strncmp(path, target, n) == 0 &&
         (path[n] == 0 || path[n] == '/');
}

void
vfs_join_path(char *out, int outsz, char *cwd, char *path)
{
  int oi = 0;
  char tmp[MAXPATH];
  char *p;

  if(out == 0 || outsz <= 0)
    return;
  if(path == 0 || path[0] == 0){
    safestrcpy(out, cwd && cwd[0] ? cwd : "/", outsz);
    return;
  }

  if(path[0] == '/')
    safestrcpy(tmp, path, sizeof(tmp));
  else if(cwd == 0 || cwd[0] == 0 || (cwd[0] == '/' && cwd[1] == 0))
    snprintf(tmp, sizeof(tmp), "/%s", path);
  else
    snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);

  out[oi++] = '/';
  p = tmp;
  while(*p && oi < outsz - 1){
    char elem[MAXPATH];
    int n = 0;

    while(*p == '/')
      p++;
    if(*p == 0)
      break;
    while(*p && *p != '/' && n < sizeof(elem) - 1)
      elem[n++] = *p++;
    elem[n] = 0;
    while(*p && *p != '/')
      p++;
    if(n == 1 && elem[0] == '.')
      continue;
    if(n == 2 && elem[0] == '.' && elem[1] == '.'){
      if(oi > 1){
        oi--;
        while(oi > 1 && out[oi - 1] != '/')
          oi--;
      }
      out[oi] = 0;
      continue;
    }
    if(oi > 1 && oi < outsz - 1)
      out[oi++] = '/';
    for(int i = 0; i < n && oi < outsz - 1; i++)
      out[oi++] = elem[i];
    out[oi] = 0;
  }
  out[oi] = 0;
}

void
vfsinit(void)
{
  initlock(&vfs_lock, "vfs");
  memset(mounts, 0, sizeof(mounts));
  memset(mount_data, 0, sizeof(mount_data));

  mounts[0].used = 1;
  mounts[0].type = VFS_XV6;
  mounts[0].flags = 0;
  mounts[0].ops = vfs_ops_for_type(VFS_XV6);
  mounts[0].private = vfs_private_for_mount(0, VFS_XV6, "xv6");
  safestrcpy(mounts[0].target, "/", sizeof(mounts[0].target));
  safestrcpy(mounts[0].source, "xv6", sizeof(mounts[0].source));

  mounts[1].used = VFS_MOUNT_READY;
  mounts[1].type = VFS_EXT4;
  mounts[1].flags = 0;
  mounts[1].ops = vfs_ops_for_type(VFS_EXT4);
  mounts[1].private = vfs_private_for_mount(1, VFS_EXT4, "ext4");
  safestrcpy(mounts[1].target, "/ext4", sizeof(mounts[1].target));
  safestrcpy(mounts[1].source, "ext4", sizeof(mounts[1].source));
}

struct vfs_mount*
vfs_root_mount(void)
{
  return &mounts[0];
}

static int
vfs_set_proc_path(struct proc_vfs_cwd *dst, char *abs_path)
{
  struct vfs_path vp;

  if(dst == 0 || abs_path == 0 || abs_path[0] != '/')
    return -1;
  if(vfs_resolve(abs_path, &vp) < 0)
    return -1;

  dst->mount = vp.mount;
  safestrcpy(dst->inner, vp.inner, sizeof(dst->inner));
  safestrcpy(dst->abs_path, abs_path, sizeof(dst->abs_path));
  return 0;
}

int
vfs_set_proc_root(struct proc *p, char *abs_path)
{
  if(p == 0)
    return -1;
  return vfs_set_proc_path(&p->vfs_root, abs_path);
}

int
vfs_set_proc_cwd(struct proc *p, char *abs_path)
{
  if(p == 0)
    return -1;
  return vfs_set_proc_path(&p->vfs_cwd, abs_path);
}

int
vfs_proc_cwd_path(struct proc *p, char *out, int outsz)
{
  char *root;
  char *cwd;
  char *suffix;
  int n;

  if(p == 0 || out == 0 || outsz <= 0)
    return -1;

  root = p->vfs_root.abs_path;
  cwd = p->vfs_cwd.abs_path;
  if(root[0] == 0 || cwd[0] == 0)
    return -1;

  if(root[0] == '/' && root[1] == 0){
    safestrcpy(out, cwd, outsz);
    return 0;
  }

  n = strlen(root);
  if(strncmp(cwd, root, n) != 0 || (cwd[n] != 0 && cwd[n] != '/'))
    return -1;

  suffix = cwd + n;
  if(suffix[0] == 0)
    safestrcpy(out, "/", outsz);
  else
    safestrcpy(out, suffix, outsz);
  return 0;
}

int
vfs_mount(char *source, char *target, char *fstype, uint64 flags)
{
  char clean[MAXPATH];
  int type, freei;
  struct vfs_mount *mnt;

  if(target == 0 || target[0] != '/')
    return -1;

  type = vfs_type_from_name(fstype);
  if(type == VFS_NONE)
    return -1;

  vfs_normalize_target(clean, sizeof(clean), target);

  acquire(&vfs_lock);
  freei = -1;
  for(int i = 0; i < VFS_MAX_MOUNTS; i++){
    if(mounts[i].used){
      if(vfs_streq(mounts[i].target, clean)){
        release(&vfs_lock);
        return -1;
      }
    } else if(freei < 0) {
      freei = i;
    }
  }

  if(freei < 0){
    release(&vfs_lock);
    return -1;
  }

  mnt = &mounts[freei];
  mnt->used = VFS_MOUNT_BUSY;
  mnt->type = type;
  mnt->flags = flags;
  mnt->ops = vfs_ops_for_type(type);
  mnt->private = vfs_private_for_mount(freei, type, source);
  if(mnt->ops == 0 || mnt->private == 0){
    memset(mnt, 0, sizeof(*mnt));
    release(&vfs_lock);
    return -1;
  }
  safestrcpy(mnt->target, clean, sizeof(mnt->target));
  if(source && source[0])
    safestrcpy(mnt->source, source, sizeof(mnt->source));
  else
    safestrcpy(mnt->source, fstype, sizeof(mnt->source));

  release(&vfs_lock);

  if(mnt->ops->mount && mnt->ops->mount(mnt, source, fstype, flags) < 0){
    acquire(&vfs_lock);
    memset(mnt, 0, sizeof(*mnt));
    release(&vfs_lock);
    return -1;
  }

  acquire(&vfs_lock);
  mnt->used = VFS_MOUNT_READY;
  release(&vfs_lock);
  return 0;
}

int
vfs_umount(char *target, int flags)
{
  char clean[MAXPATH];
  struct vfs_mount *mnt;
  int found;

  if(target == 0 || target[0] != '/')
    return -1;

  vfs_normalize_target(clean, sizeof(clean), target);
  if(vfs_streq(clean, "/"))
    return -1;

  acquire(&vfs_lock);
  found = -1;
  for(int i = 0; i < VFS_MAX_MOUNTS; i++){
    if(mounts[i].used == VFS_MOUNT_READY && vfs_streq(mounts[i].target, clean)){
      found = i;
      mounts[i].used = VFS_MOUNT_BUSY;
      break;
    }
  }

  if(found < 0){
    release(&vfs_lock);
    return -1;
  }
  mnt = &mounts[found];
  release(&vfs_lock);

  if(mnt->ops && mnt->ops->umount && mnt->ops->umount(mnt, flags) < 0){
    acquire(&vfs_lock);
    mnt->used = VFS_MOUNT_READY;
    release(&vfs_lock);
    return -1;
  }

  acquire(&vfs_lock);
  memset(mnt, 0, sizeof(*mnt));
  release(&vfs_lock);
  return 0;
}

int
vfs_resolve(char *path, struct vfs_path *out)
{
  int best = -1;
  int bestlen = -1;
  int target_len;
  char clean[MAXPATH];
  char *inner;

  if(path == 0 || out == 0 || path[0] != '/')
    return -1;
  vfs_clean_path(clean, sizeof(clean), path);
  path = clean;

  acquire(&vfs_lock);
  for(int i = 0; i < VFS_MAX_MOUNTS; i++){
    if(mounts[i].used == VFS_MOUNT_READY &&
      vfs_path_matches_mount(path, mounts[i].target)){
        int n = strlen(mounts[i].target);
        if(n > bestlen){
          best = i;
          bestlen = n;
        }
    }
  }

  if(best < 0){
    release(&vfs_lock);
    return -1;
  }

  out->type = mounts[best].type;
  out->flags = mounts[best].flags;
  out->mount = &mounts[best];
  out->ops = mounts[best].ops;
  out->private = mounts[best].private;
  safestrcpy(out->abs_path, path, sizeof(out->abs_path));
  safestrcpy(out->mountpoint, mounts[best].target, sizeof(out->mountpoint));

  target_len = strlen(mounts[best].target);
  if(vfs_streq(mounts[best].target, "/")){
    inner = path;
  } else if(path[target_len] == 0){
    inner = "/";
  } else {
    inner = path + target_len;
  }
  safestrcpy(out->inner, inner, sizeof(out->inner));

  release(&vfs_lock);
  return 0;
}

//generate the abs path
int
vfs_resolve_proc_path(struct proc *p, char *path, struct vfs_path *out)
{
  char abs[MAXPATH];

  if(p == 0 || path == 0 || out == 0)
    return -1;
  //cause we use /ext4 as the mount point
  //so we have to sperately handle the xv6 path and the ext4 path
  if(path[0] == '/'){
    if(p->vfs_root.abs_path[0] == 0)
      return -1;
    if(p->vfs_root.abs_path[0] == '/' && p->vfs_root.abs_path[1] == 0)
      return vfs_resolve(path, out);
    if(vfs_linux_passthrough_abs(path))
      return vfs_resolve(path, out);
    snprintf(abs, sizeof(abs), "%s%s", p->vfs_root.abs_path, path);
    return vfs_resolve(abs, out);
  }

  if(p->vfs_cwd.abs_path[0] == 0)
    return -1;

  vfs_join_path(abs, sizeof(abs), p->vfs_cwd.abs_path, path);
  return vfs_resolve(abs, out);
}
