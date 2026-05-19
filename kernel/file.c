//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      memset(f, 0, sizeof(*f));
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  } 
  else if(f->type == FD_EXT4){
    memset(&st, 0, sizeof(st));
    st.dev = FIRSTDEV;
    st.type = ext4_path_is_dir(FIRSTDEV, f->ext4_path) ? T_DIR : T_FILE;
    st.nlink = 1;
    st.size = ext4_file_size_by_path(FIRSTDEV, f->ext4_path);
    if(st.size == 0 && !ext4_path_is_reg(FIRSTDEV, f->ext4_path) &&
       !ext4_path_is_dir(FIRSTDEV, f->ext4_path))
      return -1;
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else if(f->type == FD_EXT4){
    if(ext4_path_is_dir(FIRSTDEV, f->ext4_path)){
      struct dirent de;
      uint64 next, ino;
      uchar type;
      char name[DIRSIZ + 1];
      int ok;

      if(n < sizeof(de))
        return -1;
      for(;;){
        ok = ext4_dirent_by_path(FIRSTDEV, f->ext4_path, f->off, &next,
                                 &ino, &type, name, sizeof(name));
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
    char *buf = kalloc();
    int m;
    if(buf == 0)
      return -1;
    m = n;
    if(m > PGSIZE)
      m = PGSIZE;
    r = ext4_read_file_by_path_at(FIRSTDEV, f->ext4_path, (uchar *)buf, m, f->off);
    if(r > 0){
      if(copyout(myproc()->pagetable, addr, buf, r) < 0)
        r = -1;
      else
        f->off += r;
    }
    kfree(buf);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else if(f->type == FD_EXT4){
    ret = -1;
  } else {
    panic("filewrite");
  }

  return ret;
}
