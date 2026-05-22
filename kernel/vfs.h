#ifndef XV6_VFS_H
#define XV6_VFS_H

#define VFS_MAX_MOUNTS 16

struct file;
struct stat;
struct vfs_mount;
struct vfs_node;

enum vfs_type {
  VFS_NONE = 0,
  VFS_XV6,
  VFS_EXT4,
  VFS_PROC,
  VFS_TMPFS,
  VFS_DEVFS,
};

struct vfs_block_mount {
  int dev;
};


struct vfs_node {
  int type;
  uint64 ino;
  uint64 mode;
  uint64 size;
  struct vfs_mount *mount;
  void *private;
};

struct vfs_super_ops {
  int  (*read_super)(struct vfs_mount*);
};

struct vfs_inode_ops {
  int (*lookup)(struct vfs_mount*, char*, struct vfs_node*);
  int (*create)(struct vfs_mount*, char*, int, int, struct vfs_node*);
  int (*mknod)(struct vfs_mount*, char*, int, int, int);
  int (*mkdir)(struct vfs_mount*, char*, int);
  int (*rmdir)(struct vfs_mount*, char*);
  int (*unlink)(struct vfs_mount*, char*);
  int (*link)(struct vfs_mount*, char*, char*);
  int (*rename)(struct vfs_mount*, char*, char*);
  int (*symlink)(struct vfs_mount*, char*, char*);
};

struct vfs_file_ops {
  int    (*open)(struct vfs_mount*, char*, int, struct file**);
  int    (*close)(struct file*);
  int    (*read)(struct file*, uint64, int);
  int    (*write)(struct file*, uint64, int);
  uint64 (*lseek)(struct file*, uint64, int);
  int    (*readdir)(struct file*, uint64, int);
  int    (*stat)(struct file*, uint64);
  int    (*mmap)(struct file*, uint64, uint64, int, int, uint64);
  //for future use
  // int    (*fsync)(struct file*);
  // int    (*ioctl)(struct file*, uint64, uint64);
};

struct vfs_ops {
  char *name;
  int (*mount)(struct vfs_mount*, char*, char*, uint64);
  int (*umount)(struct vfs_mount*, int);
  struct vfs_super_ops *super;
  struct vfs_inode_ops *inode;
  struct vfs_file_ops *file;
};

struct vfs_mount {
  int used;            // judge the item is free or not
  int type;            // the type of the file system
  uint64 flags;        // for future use
  struct vfs_ops *ops; // filesystem operations
  void *private;       // filesystem-specific mount data
  char target[MAXPATH];//the path of the mount point
  char source[MAXPATH];//the source of fs
};

//for vfs_resolve
struct vfs_path {
  int type;
  uint64 flags;
  struct vfs_mount *mount;
  struct vfs_ops *ops;
  void *private;
  char abs_path[MAXPATH];
  char mountpoint[MAXPATH];
  char inner[MAXPATH];     //the path inside the mounted file system
};

#endif
