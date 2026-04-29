// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;          //num of who want to heve spinlock
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  // Replace this with your implementation.
  uint nreader;
  uint writer_flag;
  uint waiting_writers;
  struct spinlock l;
};
#endif
