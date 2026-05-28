#ifndef LWEXT4_XV6_H
#define LWEXT4_XV6_H

#include "types.h"

struct lwext4_xv6_stat {
  uint64 mode;
  uint64 size;
  uint64 ino;
  uint64 nlink;
  uint64 blocks;
  uint uid;
  uint gid;
  uint atime;
  uint mtime;
  uint ctime;
  int type;
};

int lwext4_xv6_init(int dev);
int lwext4_xv6_stat(int dev, const char *path, struct lwext4_xv6_stat *st);
int lwext4_xv6_stat_by_path(int dev, const char *path, uint64 *mode,
                            uint64 *size, uint64 *ino, int *type);
int lwext4_xv6_read_file_by_path_at(int dev, const char *path, uchar *dst,
                                    uint32 len, uint32 off);
int lwext4_xv6_write_file_by_path_at(int dev, const char *path,
                                     const uchar *src, uint32 len,
                                     uint32 off);
int lwext4_xv6_open(int dev, const char *path, int xv6_flags, void **handle,
                    int *type, uint64 *size, uint64 *ino);
int lwext4_xv6_close(void *handle);
int lwext4_xv6_close_dir(void *handle);
int lwext4_xv6_read(void *handle, uchar *dst, uint32 len, uint64 off);
int lwext4_xv6_write(void *handle, const uchar *src, uint32 len, uint64 off);
uint64 lwext4_xv6_file_size(void *handle);
int lwext4_xv6_dirent_next(void *handle, uint64 *next, uint64 *ino,
                           uchar *type, char *name, int namesz);
int lwext4_xv6_create_file(int dev, const char *path);
int lwext4_xv6_mkdir(int dev, const char *path);
int lwext4_xv6_unlink(int dev, const char *path);
int lwext4_xv6_link(int dev, const char *oldpath, const char *newpath);
int lwext4_xv6_rename(int dev, const char *oldpath, const char *newpath);
int lwext4_xv6_symlink(int dev, const char *target, const char *path);
uint64 lwext4_xv6_file_size_by_path(int dev, const char *path);
int lwext4_xv6_path_is_dir(int dev, const char *path);
int lwext4_xv6_path_is_reg(int dev, const char *path);
int lwext4_xv6_dirent_by_path(int dev, const char *path, uint64 off,
                              uint64 *next, uint64 *ino, uchar *type,
                              char *name, int namesz);

#endif
