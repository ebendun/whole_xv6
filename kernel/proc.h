// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// Linux robust futex list head registered by set_robust_list().
struct linux_robust_list_head {
  uint64 next;
  long futex_offset;
  uint64 pending;
};

// Per-process data for the trap handling code in trampoline.S.
//
// This page sits just below the trampoline page in the user page table,
// and is not specially mapped in the kernel page table.  uservec saves
// user registers here, loads kernel_* state, and jumps to usertrap().
// usertrapret() and userret restore these registers and return to user
// space.  Keep the offsets below in sync with trampoline.S.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate {
  UNUSED,
  USED,
  SLEEPING,
  RUNNABLE,
  RUNNING,
  ZOMBIE,
};

struct usyscall;

struct vma {
  int used;
  int prot;
  int flags;

  uint64 addr;
  uint64 len;
  uint64 filelen;
  uint64 offset;

  struct file *f;
};

struct vfs_mount;

struct linux_mm {
  struct spinlock lock;
  int refcnt;

  pagetable_t pagetable;
  uint64 sz;
  uint64 linux_brk;
  uint64 linux_brk_limit;
  uint64 mmap_base;

  struct vma vmas[NVMA];
};

struct linux_thread_group {
  struct spinlock lock;
  int refcnt;

  int tgid;
  int exiting;
  int xstate;
  int thread_count;

  struct proc *leader;
  struct proc *members;
};

struct proc_vfs_path {
  struct vfs_mount *mount;
  char inner[MAXPATH];
  char abs_path[MAXPATH];
};

// Per-process state.
struct proc {
  struct spinlock lock;

  // Protected by p->lock.
  enum procstate state;       // Process state.
  void *chan;                 // Sleep channel, or zero.
  int killed;                 // Non-zero if killed.
  int xstate;                 // Exit status returned to wait().
  int pid;                    // Process ID.

  uint futex_bitset;          // FUTEX_WAIT_BITSET mask.
  uint futex_deadline;        // Tick deadline for timed futex sleep.
  int futex_timedout;         // Timed futex sleep expired.

  // Protected by wait_lock.
  struct proc *parent;        // Parent process.

  // Private to this process; p->lock is not required.
  uint64 kstack;              // Virtual address of kernel stack.
  pagetable_t pagetable;      // User page table.
  struct trapframe *trapframe; // Data page for trampoline.S.
  uint64 trapframe_va;        // Per-thread trapframe slot in user page table.
  struct context context;     // swtch() here to run process.

  struct file *ofile[NOFILE]; // Open files.
  int ofd_flags[NOFILE];      // Per-fd flags such as FD_CLOEXEC.
  struct inode *cwd;          // Current directory.
  struct proc_vfs_path vfs_root; // Process root in VFS terms.
  struct proc_vfs_path vfs_cwd;  // Current directory in VFS terms.
  char name[16];              // Process name for debugging.
  struct cpu *pincpu;

  // Linux ABI state.
  int is_linux;               // Running a Linux ABI image.
  struct linux_mm *mm;        // Shared address-space metadata.
  char linux_exe_path[MAXPATH]; // Absolute Linux-visible executable path.

  // Linux clone/thread sharing.
  int linux_share_vm;         // CLONE_VM-style shared user memory.
  int linux_share_files;      // CLONE_FILES-style shared fd table.
  int linux_share_fs;         // CLONE_FS-style shared cwd/root state.
  int linux_is_thread;        // CLONE_THREAD-style task.
  struct linux_thread_group *linux_group;
  struct proc *linux_group_next;

  // Linux signals.
  int linux_signal_pending;   // Interrupt blocking syscalls.
  int linux_pending_signal;   // Signal to deliver before user return.
  int linux_pending_sender;   // Sender pid for pending signal.
  int linux_in_signal;        // Currently running a signal handler.
  uint64 linux_rt_signal_handler;
  uint64 linux_sigmask;       // Blocked signal mask.
  char *sigreturn;            // User executable rt_sigreturn stub.

  // Linux futex cleanup.
  uint64 clear_child_tid;     // CLONE_CHILD_CLEARTID futex address.
  uint64 robust_list;         // Robust futex list head.
  uint64 robust_list_len;     // Length supplied to set_robust_list().

  // Page-table lab.
  struct usyscall *usyscall;  // Shared user/kernel page.

  // Syscall lab.
  int interpose_mask;         // Bit mask of blocked syscalls.
  char interpose_path[MAXPATH]; // Allowed path for masked open/exec.

  // Trap lab.
  struct trapframe alarm_trapframe; // Saved user registers for sigalarm.
  int alarm_interval;         // Ticks between alarms.
  int alarm_ticks;            // Ticks since last alarm.
  uint64 alarm_handler;       // User handler pc.
  int alarm_inflight;         // Handler active.
};
