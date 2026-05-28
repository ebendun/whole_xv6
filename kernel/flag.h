#ifndef XV6_LINUX_FLAGS_H
#define XV6_LINUX_FLAGS_H

// Linux clone flags used by the compatibility layer.
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

// Linux futex flag bits used by the compatibility layer.
#define FUTEX_PRIVATE_FLAG   0x80
#define FUTEX_CLOCK_REALTIME 0x100

#endif
