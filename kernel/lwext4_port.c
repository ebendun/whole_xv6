#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "lwext4_xv6.h"

#define CONFIG_USE_DEFAULT_CFG 1
#define CONFIG_USE_USER_MALLOC 1
#define CONFIG_HAVE_OWN_ERRNO 1
#define CONFIG_HAVE_OWN_OFLAGS 1
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0
#define CONFIG_JOURNALING_ENABLE 0
#define CONFIG_XATTR_ENABLE 0
#define CONFIG_EXT4_MAX_MP_NAME 4
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 1
#define CONFIG_BLOCK_DEV_CACHE_SIZE 16

#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_oflags.h>
#include <ext4_types.h>

#define LWEXT4_MOUNTPOINT "/"
#define LWEXT4_VFS_PREFIX "/ext4"
#define LWEXT4_ALLOCS 256
#define XV6_O_WRONLY 0x001
#define XV6_O_RDWR   0x002
#define XV6_O_CREATE 0x200
#define XV6_O_TRUNC  0x400

struct lwext4_alloc {
  void *ptr;
  uint size;
};

static struct spinlock lwext4_alloc_lock;
static struct sleeplock lwext4_fs_lock;
static int lwext4_lock_inited;
static int lwext4_mounted;
static int lwext4_dev = -1;
static uchar lwext4_devbuf[BSIZE];
static struct ext4_blockdev_iface lwext4_iface;
static struct ext4_blockdev lwext4_bdev;
static struct lwext4_alloc lwext4_allocs[LWEXT4_ALLOCS];

static int lwext4_stat_full_locked(const char *full,
                                   struct lwext4_xv6_stat *st);

static int
lwext4_map_open_flags(int xv6_flags)
{
  int flags;

  switch(xv6_flags & 3){
  case XV6_O_WRONLY:
    flags = O_WRONLY;
    break;
  case XV6_O_RDWR:
    flags = O_RDWR;
    break;
  default:
    flags = O_RDONLY;
    break;
  }
  if(xv6_flags & XV6_O_CREATE)
    flags |= O_CREAT;
  if(xv6_flags & XV6_O_TRUNC)
    flags |= O_TRUNC;
  return flags;
}

static int
lwext4_clean_path(char *dst, int dstsz, const char *path)
{
  int di = 1;
  int si = 0;

  if(dst == 0 || dstsz < 2 || path == 0)
    return -1;

  if(strncmp(path, LWEXT4_VFS_PREFIX, strlen(LWEXT4_VFS_PREFIX)) == 0 &&
     (path[strlen(LWEXT4_VFS_PREFIX)] == 0 ||
      path[strlen(LWEXT4_VFS_PREFIX)] == '/')){
    path += strlen(LWEXT4_VFS_PREFIX);
    if(path[0] == 0)
      path = "/";
  }

  dst[0] = '/';
  dst[1] = 0;
  while(path[si]){
    char elem[MAXPATH];
    int n = 0;

    while(path[si] == '/')
      si++;
    if(path[si] == 0)
      break;

    while(path[si] && path[si] != '/' && n < sizeof(elem) - 1)
      elem[n++] = path[si++];
    elem[n] = 0;
    while(path[si] && path[si] != '/')
      si++;

    if(n == 0 || (n == 1 && elem[0] == '.'))
      continue;
    if(n == 2 && elem[0] == '.' && elem[1] == '.'){
      if(di > 1){
        di--;
        while(di > 1 && dst[di - 1] != '/')
          di--;
      }
      dst[di] = 0;
      continue;
    }

    if(di > 1){
      if(di >= dstsz - 1)
        return -1;
      dst[di++] = '/';
    }
    for(int i = 0; i < n; i++){
      if(di >= dstsz - 1)
        return -1;
      dst[di++] = elem[i];
    }
    dst[di] = 0;
  }

  if(di == 0)
    dst[di++] = '/';
  dst[di] = 0;
  return 0;
}

void ext4_user_free(void *ptr);

void*
ext4_user_malloc(unsigned long size)
{
  void *p;

  if(size == 0 || size > PGSIZE)
    return 0;
  p = kalloc();
  if(p == 0)
    return 0;

  acquire(&lwext4_alloc_lock);
  for(int i = 0; i < LWEXT4_ALLOCS; i++){
    if(lwext4_allocs[i].ptr == 0){
      lwext4_allocs[i].ptr = p;
      lwext4_allocs[i].size = size;
      release(&lwext4_alloc_lock);
      return p;
    }
  }
  release(&lwext4_alloc_lock);

  kfree(p);
  return 0;
}

void*
ext4_user_calloc(unsigned long count, unsigned long size)
{
  unsigned long total = count * size;
  void *p;

  if(size != 0 && total / size != count)
    return 0;
  p = ext4_user_malloc(total);
  if(p)
    memset(p, 0, total);
  return p;
}

void*
ext4_user_realloc(void *ptr, unsigned long size)
{
  void *newp;
  uint oldsize = 0;
  uint n;

  if(ptr == 0)
    return ext4_user_malloc(size);
  if(size == 0){
    ext4_user_free(ptr);
    return 0;
  }

  acquire(&lwext4_alloc_lock);
  for(int i = 0; i < LWEXT4_ALLOCS; i++){
    if(lwext4_allocs[i].ptr == ptr){
      oldsize = lwext4_allocs[i].size;
      break;
    }
  }
  release(&lwext4_alloc_lock);
  if(oldsize == 0)
    return 0;

  newp = ext4_user_malloc(size);
  if(newp == 0)
    return 0;
  n = oldsize < size ? oldsize : size;
  memmove(newp, ptr, n);
  ext4_user_free(ptr);
  return newp;
}

void
ext4_user_free(void *ptr)
{
  if(ptr == 0)
    return;

  acquire(&lwext4_alloc_lock);
  for(int i = 0; i < LWEXT4_ALLOCS; i++){
    if(lwext4_allocs[i].ptr == ptr){
      lwext4_allocs[i].ptr = 0;
      lwext4_allocs[i].size = 0;
      release(&lwext4_alloc_lock);
      kfree(ptr);
      return;
    }
  }
  release(&lwext4_alloc_lock);
}

static int
lwext4_open(struct ext4_blockdev *bdev)
{
  (void)bdev;
  return EOK;
}

static int
lwext4_close(struct ext4_blockdev *bdev)
{
  (void)bdev;
  return EOK;
}

static int
lwext4_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
             uint32_t blk_cnt)
{
  uchar *dst = (uchar*)buf;
  uint64_t base = bdev->part_offset / BSIZE;

  for(uint32_t i = 0; i < blk_cnt; i++){
    struct buf *b = bread(lwext4_dev, base + blk_id + i);
    memmove(dst + i * BSIZE, b->data, BSIZE);
    brelse(b);
  }
  return EOK;
}

static int
lwext4_bwrite(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id,
              uint32_t blk_cnt)
{
  const uchar *src = (const uchar*)buf;
  uint64_t base = bdev->part_offset / BSIZE;

  for(uint32_t i = 0; i < blk_cnt; i++){
    struct buf *b = bread(lwext4_dev, base + blk_id + i);
    memmove(b->data, src + i * BSIZE, BSIZE);
    bwrite(b);
    brelse(b);
  }
  return EOK;
}

int
lwext4_xv6_init(int dev)
{
  int r;

  if(!lwext4_lock_inited){
    initlock(&lwext4_alloc_lock, "lwext4_alloc");
    initsleeplock(&lwext4_fs_lock, "lwext4_fs");
    lwext4_lock_inited = 1;
  }
  if(lwext4_mounted)
    return 0;

  lwext4_dev = dev;
  memset(&lwext4_iface, 0, sizeof(lwext4_iface));
  memset(&lwext4_bdev, 0, sizeof(lwext4_bdev));
  lwext4_iface.open = lwext4_open;
  lwext4_iface.bread = lwext4_bread;
  lwext4_iface.bwrite = lwext4_bwrite;
  lwext4_iface.close = lwext4_close;
  lwext4_iface.ph_bsize = BSIZE;
  lwext4_iface.ph_bcnt = 0x100000;
  lwext4_iface.ph_bbuf = lwext4_devbuf;
  lwext4_bdev.bdif = &lwext4_iface;
  lwext4_bdev.part_offset = 0;
  lwext4_bdev.part_size = (uint64)lwext4_iface.ph_bcnt * BSIZE;

  ext4_dmask_set(0);
  r = ext4_mount(&lwext4_bdev, LWEXT4_MOUNTPOINT, false);
  if(r != EOK)
    return -1;
  lwext4_mounted = 1;
  return 0;
}

int
lwext4_xv6_stat(int dev, const char *path, struct lwext4_xv6_stat *st)
{
  char full[MAXPATH];
  int r;

  if(st == 0)
    return -1;
  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  r = lwext4_stat_full_locked(full, st);
  releasesleep(&lwext4_fs_lock);
  return r;
}

int
lwext4_xv6_stat_by_path(int dev, const char *path, uint64 *mode,
                        uint64 *size, uint64 *ino, int *type)
{
  struct lwext4_xv6_stat st;

  if(lwext4_xv6_stat(dev, path, &st) < 0)
    return -1;
  if(mode) *mode = st.mode;
  if(size) *size = st.size;
  if(ino) *ino = st.ino;
  if(type) *type = st.type;
  return 0;
}

static int
lwext4_stat_full_locked(const char *full, struct lwext4_xv6_stat *st)
{
  struct ext4_inode_ref ref;
  struct ext4_mountpoint *mp;
  uint32 mode;
  uint32 kind;

  memset(st, 0, sizeof(*st));
  mp = ext4_get_inode_ref(full, &ref);
  if(mp == 0)
    return -1;

  mode = ext4_inode_get_mode(&ref.fs->sb, ref.inode);
  kind = mode & EXT4_INODE_MODE_TYPE_MASK;
  st->mode = mode;
  st->size = ext4_inode_get_size(&ref.fs->sb, ref.inode);
  st->ino = ref.index;
  st->nlink = ext4_inode_get_links_cnt(ref.inode);
  st->blocks = ext4_inode_get_blocks_count(&ref.fs->sb, ref.inode);
  st->uid = ext4_inode_get_uid(ref.inode);
  st->gid = ext4_inode_get_gid(ref.inode);
  st->atime = ext4_inode_get_access_time(ref.inode);
  st->mtime = ext4_inode_get_modif_time(ref.inode);
  st->ctime = ext4_inode_get_change_inode_time(ref.inode);
  if(kind == EXT4_INODE_MODE_DIRECTORY)
    st->type = T_DIR;
  else if(kind == EXT4_INODE_MODE_FILE || kind == EXT4_INODE_MODE_SOFTLINK)
    st->type = T_FILE;
  else
    st->type = T_DEVICE;

  ext4_put_inode_ref(mp, &ref);
  return 0;
}

int
lwext4_xv6_read_file_by_path_at(int dev, const char *path, uchar *dst,
                                uint32 len, uint32 off)
{
  char full[MAXPATH];
  ext4_file f;
  size_t rcnt = 0;
  int ret = -1;

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  memset(&f, 0, sizeof(f));
  if(ext4_fopen2(&f, full, O_RDONLY) != EOK)
    goto out;
  if(ext4_fseek(&f, off, SEEK_SET) != EOK){
    ext4_fclose(&f);
    goto out;
  }
  if(ext4_fread(&f, dst, len, &rcnt) != EOK){
    ext4_fclose(&f);
    goto out;
  }
  ext4_fclose(&f);
  ret = rcnt;
out:
  releasesleep(&lwext4_fs_lock);
  return ret;
}

int
lwext4_xv6_write_file_by_path_at(int dev, const char *path, const uchar *src,
                                 uint32 len, uint32 off)
{
  char full[MAXPATH];
  ext4_file f;
  size_t wcnt = 0;
  int ret = -1;

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  memset(&f, 0, sizeof(f));
  if(ext4_fopen2(&f, full, O_RDWR) != EOK)
    goto out;
  if(ext4_fseek(&f, off, SEEK_SET) != EOK){
    ext4_fclose(&f);
    goto out;
  }
  if(ext4_fwrite(&f, src, len, &wcnt) != EOK){
    ext4_fclose(&f);
    goto out;
  }
  ext4_fclose(&f);
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  ret = wcnt;
out:
  releasesleep(&lwext4_fs_lock);
  return ret;
}

int
lwext4_xv6_open(int dev, const char *path, int xv6_flags, void **handle,
                int *type, uint64 *size, uint64 *ino)
{
  char full[MAXPATH];
  ext4_file *f;
  ext4_dir *d;
  int r;

  if(handle == 0)
    return -1;
  *handle = 0;
  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  f = ext4_user_malloc(sizeof(*f));
  if(f == 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  memset(f, 0, sizeof(*f));
  r = ext4_fopen2(f, full, lwext4_map_open_flags(xv6_flags));
  if(r == EOK){
    *handle = f;
    if(type) *type = T_FILE;
    if(size) *size = ext4_fsize(f);
    if(ino) *ino = f->inode;
    releasesleep(&lwext4_fs_lock);
    return 0;
  }
  ext4_user_free(f);

  d = ext4_user_malloc(sizeof(*d));
  if(d == 0){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  memset(d, 0, sizeof(*d));
  if((xv6_flags & 3) == 0 && ext4_dir_open(d, full) == EOK){
    *handle = d;
    if(type) *type = T_DIR;
    if(size) *size = 0;
    if(ino) *ino = d->f.inode;
    releasesleep(&lwext4_fs_lock);
    return 0;
  }
  ext4_user_free(d);

  releasesleep(&lwext4_fs_lock);
  return -1;
}

int
lwext4_xv6_close(void *handle)
{
  ext4_file *f = (ext4_file*)handle;

  if(f == 0)
    return 0;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_fclose(f) != EOK){
    releasesleep(&lwext4_fs_lock);
    ext4_user_free(f);
    return -1;
  }
  releasesleep(&lwext4_fs_lock);
  ext4_user_free(f);
  return 0;
}

int
lwext4_xv6_close_dir(void *handle)
{
  ext4_dir *d = (ext4_dir*)handle;

  if(d == 0)
    return 0;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_dir_close(d) != EOK){
    releasesleep(&lwext4_fs_lock);
    ext4_user_free(d);
    return -1;
  }
  releasesleep(&lwext4_fs_lock);
  ext4_user_free(d);
  return 0;
}

int
lwext4_xv6_dirent_next(void *handle, uint64 *next, uint64 *ino, uchar *type,
                       char *name, int namesz)
{
  ext4_dir *d = (ext4_dir*)handle;
  const ext4_direntry *de;

  if(d == 0 || namesz <= 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  for(;;){
    de = ext4_dir_entry_next(d);
    if(de == 0){
      releasesleep(&lwext4_fs_lock);
      return 0;
    }
    if(de->inode == 0)
      continue;
    if(de->name_length == 1 && de->name[0] == '.')
      continue;
    if(de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.')
      continue;

    int n = de->name_length;
    if(n >= namesz)
      n = namesz - 1;
    memmove(name, de->name, n);
    name[n] = 0;
    if(next)
      *next = d->next_off == (uint64)-1 ? d->next_off : d->next_off + 1;
    if(ino)
      *ino = de->inode;
    if(type)
      *type = de->inode_type;
    releasesleep(&lwext4_fs_lock);
    return 1;
  }
}

int
lwext4_xv6_read(void *handle, uchar *dst, uint32 len, uint64 off)
{
  ext4_file *f = (ext4_file*)handle;
  size_t rcnt = 0;

  if(f == 0 || dst == 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_fseek(f, off, SEEK_SET) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  if(ext4_fread(f, dst, len, &rcnt) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  releasesleep(&lwext4_fs_lock);
  return rcnt;
}

int
lwext4_xv6_write(void *handle, const uchar *src, uint32 len, uint64 off)
{
  ext4_file *f = (ext4_file*)handle;
  size_t wcnt = 0;

  if(f == 0 || src == 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_fseek(f, off, SEEK_SET) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  if(ext4_fwrite(f, src, len, &wcnt) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return wcnt;
}

uint64
lwext4_xv6_file_size(void *handle)
{
  ext4_file *f = (ext4_file*)handle;

  if(f == 0)
    return 0;
  acquiresleep(&lwext4_fs_lock);
  uint64 size = ext4_fsize(f);
  releasesleep(&lwext4_fs_lock);
  return size;
}

int
lwext4_xv6_create_file(int dev, const char *path)
{
  char full[MAXPATH];
  ext4_file f;
  int ret = -1;

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  memset(&f, 0, sizeof(f));
  if(ext4_fopen2(&f, full, O_RDWR | O_CREAT) != EOK)
    goto out;
  ext4_fclose(&f);
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  ret = 0;
out:
  releasesleep(&lwext4_fs_lock);
  return ret;
}

int
lwext4_xv6_mkdir(int dev, const char *path)
{
  char full[MAXPATH];

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_dir_mk(full) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return 0;
}

int
lwext4_xv6_unlink(int dev, const char *path)
{
  char full[MAXPATH];
  struct lwext4_xv6_stat st;

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(lwext4_stat_full_locked(full, &st) < 0){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  if(st.type == T_DIR){
    if(ext4_dir_rm(full) != EOK){
      releasesleep(&lwext4_fs_lock);
      return -1;
    }
  } else if(ext4_fremove(full) != EOK) {
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return 0;
}

int
lwext4_xv6_link(int dev, const char *oldpath, const char *newpath)
{
  char oldfull[MAXPATH];
  char newfull[MAXPATH];

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(oldfull, sizeof(oldfull), oldpath) < 0 ||
     lwext4_clean_path(newfull, sizeof(newfull), newpath) < 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_flink(oldfull, newfull) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return 0;
}

int
lwext4_xv6_rename(int dev, const char *oldpath, const char *newpath)
{
  char oldfull[MAXPATH];
  char newfull[MAXPATH];

  if(lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(oldfull, sizeof(oldfull), oldpath) < 0 ||
     lwext4_clean_path(newfull, sizeof(newfull), newpath) < 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_frename(oldfull, newfull) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return 0;
}

int
lwext4_xv6_symlink(int dev, const char *target, const char *path)
{
  char full[MAXPATH];

  if(lwext4_xv6_init(dev) < 0 || target == 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;
  acquiresleep(&lwext4_fs_lock);
  if(ext4_fsymlink(target, full) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }
  ext4_cache_flush(LWEXT4_MOUNTPOINT);
  releasesleep(&lwext4_fs_lock);
  return 0;
}

uint64
lwext4_xv6_file_size_by_path(int dev, const char *path)
{
  uint64 size = 0;
  if(lwext4_xv6_stat_by_path(dev, path, 0, &size, 0, 0) < 0)
    return 0;
  return size;
}

int
lwext4_xv6_path_is_dir(int dev, const char *path)
{
  int type = 0;
  return lwext4_xv6_stat_by_path(dev, path, 0, 0, 0, &type) == 0 &&
         type == T_DIR;
}

int
lwext4_xv6_path_is_reg(int dev, const char *path)
{
  int type = 0;
  return lwext4_xv6_stat_by_path(dev, path, 0, 0, 0, &type) == 0 &&
         type == T_FILE;
}

int
lwext4_xv6_dirent_by_path(int dev, const char *path, uint64 off,
                          uint64 *next, uint64 *ino, uchar *type,
                          char *name, int namesz)
{
  char full[MAXPATH];
  ext4_dir d;
  const ext4_direntry *de;
  uint64 idx = 0;

  if(namesz <= 0 || lwext4_xv6_init(dev) < 0)
    return -1;
  if(lwext4_clean_path(full, sizeof(full), path) < 0)
    return -1;

  acquiresleep(&lwext4_fs_lock);
  memset(&d, 0, sizeof(d));
  if(ext4_dir_open(&d, full) != EOK){
    releasesleep(&lwext4_fs_lock);
    return -1;
  }

  while((de = ext4_dir_entry_next(&d)) != 0){
    if(de->inode == 0)
      continue;
    if(de->name_length == 1 && de->name[0] == '.')
      continue;
    if(de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.')
      continue;
    if(idx++ < off)
      continue;

    int n = de->name_length;
    if(n >= namesz)
      n = namesz - 1;
    memmove(name, de->name, n);
    name[n] = 0;
    if(next)
      *next = idx;
    if(ino)
      *ino = de->inode;
    if(type)
      *type = de->inode_type;
    ext4_dir_close(&d);
    releasesleep(&lwext4_fs_lock);
    return 1;
  }

  if(next)
    *next = idx;
  ext4_dir_close(&d);
  releasesleep(&lwext4_fs_lock);
  return 0;
}
