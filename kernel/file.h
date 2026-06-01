#include "param.h"

#define SOCKET_QUEUE 8
#define SOCKET_PAYLOAD 512

struct vfs_ops;

// Minimal IPv4 sockaddr used by the Linux socket compatibility layer.
// Values are stored in the Linux ABI layout so they can be copied directly
// to and from user-space sockaddr_in buffers.
struct linux_sockaddr_in {
  ushort family;
  ushort port;
  uint addr;
  char zero[8];
} __attribute__((packed));

// In-memory datagram payload queued on an FD_SOCKET file.
struct linux_sockpkt {
  int len;
  struct linux_sockaddr_in from;
  char data[SOCKET_PAYLOAD];
};

// Open file table entry.
//
// A struct file is shared by duplicated file descriptors and forked children;
// ref counts the references to this open-file description.  The readable,
// writable, status_flags, and off fields describe descriptor-visible state,
struct file {
  // Common open-file state.
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_EXT4, FD_SOCKET } type;
  int ref;                   // References from fd tables and local users.
  char readable;             // File may be read through fileread().
  char writable;             // File may be written through filewrite().
  struct vfs_ops *vfs_ops;   // Filesystem dispatch table for VFS-backed files.
  int status_flags;          // Open-file flags such as O_APPEND/O_NONBLOCK.

  // Cached timestamps supplied by utimensat-style operations when the backing
  // filesystem cannot store the requested values directly.
  int has_time;
  uint64 atime_sec;
  uint64 atime_nsec;
  uint64 mtime_sec;
  uint64 mtime_nsec;

  // FD_PIPE state.
  struct pipe *pipe;

  // FD_INODE and FD_DEVICE state for the xv6 filesystem/device layer.
  struct inode *ip;
  uint64 off;                // Current file offset for seekable files.

  // FD_EXT4 and other VFS-backed filesystem state.
  void *fs_file;             // Filesystem-private open handle.
  int fs_is_dir;             // Non-zero when fs_file is a directory handle.
  char ext4_path[MAXPATH];   // Absolute path used by the ext4 backend.

  // FD_DEVICE state.
  short major;               // Device major number when type == FD_DEVICE.

  // FD_SOCKET state for the minimal Linux AF_INET compatibility layer.
  int sock_domain;
  int sock_type;             // SOCK_STREAM/SOCK_DGRAM plus creation flags.
  int sock_proto;
  int sock_listening;        // Stream socket is accepting connections.
  int sock_connected;        // Socket has a selected peer.
  int sock_pending;          // Listener has a pending synthetic connection.
  struct linux_sockaddr_in sock_local; // Bound local address.
  struct linux_sockaddr_in sock_peer;  // Connected peer or pending client.
  struct linux_sockpkt sock_q[SOCKET_QUEUE]; // Datagram receive queue.
  int sock_qhead;            // Next packet to receive.
  int sock_qtail;            // Next free packet slot.
  int sock_qcount;           // Number of queued packets.
};

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
#define DEVNULL 3
#define DEVZERO 4
