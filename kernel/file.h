#include "param.h"
#include "ext4.h"

#define SOCKET_QUEUE 8
#define SOCKET_PAYLOAD 512

struct linux_sockaddr_in {
  ushort family;
  ushort port;
  uint addr;
  char zero[8];
} __attribute__((packed));

struct linux_sockpkt {
  int len;
  struct linux_sockaddr_in from;
  char data[SOCKET_PAYLOAD];
};

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_EXT4, FD_SOCKET } type;
  int ref; // reference count
  char readable;
  char writable;
  int fd_flags;
  int status_flags;
  int has_time;
  uint64 atime_sec;
  uint64 atime_nsec;
  uint64 mtime_sec;
  uint64 mtime_nsec;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint64 off;        // FD_INODE / FD_EXT4
  char ext4_path[MAXPATH]; // FD_EXT4
  short major;       // FD_DEVICE
  int sock_domain;
  int sock_type;
  int sock_proto;
  int sock_listening;
  int sock_connected;
  int sock_pending;
  struct linux_sockaddr_in sock_local;
  struct linux_sockaddr_in sock_peer;
  struct linux_sockpkt sock_q[SOCKET_QUEUE];
  int sock_qhead;
  int sock_qtail;
  int sock_qcount;
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+1];
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
#define STATS   2
#define DEVNULL 3
#define DEVZERO 4
