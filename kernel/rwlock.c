// Mutual exclusion read-write locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "rwlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"


static void
read_acquire_inner(struct rwspinlock *rwlk)
{
  acquire(&rwlk->l);
  for(;;){
    if(rwlk->writer_flag == 0 && rwlk->waiting_writers == 0){
      rwlk->nreader += 1;
      release(&rwlk->l);
      return;
    }
    release(&rwlk->l);
    acquire(&rwlk->l);
  }
}

static void
read_release_inner(struct rwspinlock *rwlk)
{
  acquire(&rwlk->l);
  if(rwlk->nreader < 1)
    panic("read_release_inner");
  
  rwlk->nreader -= 1;
  release(&rwlk->l);
}

static void
write_acquire_inner(struct rwspinlock *rwlk)
{
  acquire(&rwlk->l);
  rwlk->waiting_writers += 1;
  while(rwlk->writer_flag || rwlk->nreader){
    release(&rwlk->l);
    acquire(&rwlk->l);
  }
  rwlk->waiting_writers -= 1;
  rwlk->writer_flag = 1;
  release(&rwlk->l);
}

static void
write_release_inner(struct rwspinlock *rwlk)
{
  // Replace this with your implementation.
  acquire(&rwlk->l);
  if(rwlk->writer_flag == 0)
    panic("write_release_inner");
  rwlk->writer_flag = 0;
  release(&rwlk->l);
}

void
read_acquire(struct rwspinlock *rwlk)
{
  push_off(); // disable interrupts to avoid deadlock.
  read_acquire_inner(rwlk);
}

void
read_release(struct rwspinlock *rwlk)
{
  read_release_inner(rwlk);
  pop_off();
}

void
write_acquire(struct rwspinlock *rwlk)
{
  push_off(); // disable interrupts to avoid deadlock.
  write_acquire_inner(rwlk);
}

void
write_release(struct rwspinlock *rwlk)
{
  write_release_inner(rwlk);
  pop_off();
}

void
initrwlock(struct rwspinlock *rwlk)
{
  // Replace this with your implementation.
  rwlk->nreader = 0;
  rwlk->writer_flag = 0;
  rwlk->waiting_writers = 0;
  initlock(&rwlk->l, "rwlk");
}