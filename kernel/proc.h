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

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
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

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct usyscall;

struct vma {
  int used;
  uint64 addr;
  uint64 len;
  uint64 filelen;
  int prot;
  int flags;
  uint64 offset;
  struct file *f;
};

struct vfs_mount;

struct proc_vfs_cwd {
  struct vfs_mount *mount;
  char inner[MAXPATH];
  char abs_path[MAXPATH];
};


// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  uint futex_deadline;         // tick deadline for timed futex sleep
  int futex_timedout;          // timed futex sleep expired
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  int is_linux;                // Process is running a Linux ABI image
  int linux_signal_pending;    // Linux-ABI signal interrupt for blocking syscalls
  int linux_pending_signal;    // Linux signal to deliver before user return
  int linux_pending_sender;    // Sender pid for pending Linux signal
  int linux_in_signal;         // currently running a Linux signal handler
  int linux_share_vm;          // Linux CLONE_VM-style shared user memory
  int linux_share_files;       // Linux CLONE_FILES-style shared fd table
  int linux_share_fs;          // Linux CLONE_FS-style shared cwd/root state
  int linux_is_thread;         // Linux CLONE_THREAD-style task
  int linux_tgid;              // Linux thread-group id; pid for normal procs
  int linux_group_exiting;     // thread group is being torn down
  int linux_group_xstate;      // status supplied to exit_group
  int linux_thread_count;      // live tasks, meaningful on group leader
  struct proc *linux_group_leader; // leader of this Linux thread group
  uint64 linux_sigcancel_handler; // handler installed for Linux SIGCANCEL
  uint64 linux_sigmask;        // Linux blocked signal mask
  uint64 linux_brk;            // Current Linux ABI program break
  uint64 linux_brk_limit;      // Highest Linux brk before the stack guard
  char linux_exe_path[MAXPATH]; // Absolute Linux-visible executable path
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct usyscall *usyscall;   // shared user/kernel page
  char *sigreturn;             // user executable rt_sigreturn stub
  struct trapframe alarm_trapframe; // saved user registers for sigalarm
  int alarm_interval;          // ticks between alarms
  int alarm_ticks;             // ticks since last alarm
  uint64 alarm_handler;        // user handler pc
  int alarm_inflight;          // handler active
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  struct proc_vfs_cwd vfs_root;// Process root in VFS terms
  struct proc_vfs_cwd vfs_cwd; // Current directory in VFS terms
  char name[16];               // Process name (debugging)
  int interpose_mask;          // Bit mask of blocked syscalls
  char interpose_path[MAXPATH];// Allowed path for masked open/exec
  struct cpu *pincpu;
  uint64 mmap_base;            // next mmap allocation address (grows down)
  struct vma vmas[NVMA];
  uint64 clear_child_tid;      // Linux CLONE_CHILD_CLEARTID futex address
};
