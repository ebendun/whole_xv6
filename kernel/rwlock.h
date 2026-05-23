// Reader-writer lock.
#include "spinlock.h"

struct rwspinlock {
  // Replace this with your implementation.
  uint nreader;
  uint writer_flag;
  uint waiting_writers;
  struct spinlock l;
};