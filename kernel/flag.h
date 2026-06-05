#ifndef XV6_LINUX_FLAGS_H
#define XV6_LINUX_FLAGS_H

// Linux clone flags used by the compatibility layer.

//share the same mem
#define CLONE_VM             0x00000100
//share the same cwd and root dir
#define CLONE_FS             0x00000200
//share the same file descriptor table
#define CLONE_FILES          0x00000400
//parent and child belong the same thread group
#define CLONE_THREAD         0x00010000
//child own private addr
#define CLONE_SETTLS         0x00080000
//parent get child tid
#define CLONE_PARENT_SETTID  0x00100000
//child get tid
#define CLONE_CHILD_CLEARTID 0x00200000
//for futex, wake up only one waiting thread
#define CLONE_CHILD_SETTID   0x01000000

// Linux futex flag bits used by the compatibility layer.
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAKE_OP        5
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10

#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

#define FUTEX_PRIVATE_FLAG   0x80
#define FUTEX_CLOCK_REALTIME 0x100

struct linux_timespec {
  uint64 sec;   // second
  uint64 nsec;  // nanosecond
};

//low 30 bit
#define FUTEX_TID_MASK       0x3fffffff
//set it to dead
#define FUTEX_OWNER_DIED     0x40000000

#endif
