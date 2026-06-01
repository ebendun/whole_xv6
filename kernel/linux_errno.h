// Linux errno values returned as negative syscall results.
#define LINUX_ENOENT    2         // No such file or directory.
#define LINUX_ESRCH     3         // No such process.
#define LINUX_EINTR     4         // Interrupted by a signal.
#define LINUX_EBADF     9         // Bad file descriptor.
#define LINUX_EAGAIN    11        // Operation would block; try again.
#define LINUX_ENOMEM    12        // Out of memory.
#define LINUX_EACCES    13        // Permission denied.
#define LINUX_EFAULT    14        // Bad user-space address.
#define LINUX_ENOTDIR   20        // Path component is not a directory.
#define LINUX_EINVAL    22        // Invalid argument.
#define LINUX_EMFILE    24        // Per-process file descriptor limit reached.
#define LINUX_ESPIPE    29        // Illegal seek on non-seekable file.
#define LINUX_EPIPE     32        // Broken pipe.
#define LINUX_ENOSYS    38        // Syscall or operation not implemented.
#define LINUX_ESOCKTNOSUPPORT 94  // Unsupported socket type.
#define LINUX_EOPNOTSUPP 95       // Operation not supported.
#define LINUX_EAFNOSUPPORT 97     // Unsupported address family.
#define LINUX_EADDRINUSE 98       // Address already in use.
#define LINUX_ETIMEDOUT 110       // Operation timed out.
#define LINUX_ECONNREFUSED 111    // Connection refused.
