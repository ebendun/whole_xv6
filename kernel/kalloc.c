// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    uint64 base = PGROUNDUP((uint64)end);
    uint64 total_mem = PHYSTOP - base;
    uint64 piece = total_mem / NCPU;
    uint64 s = base + i * piece;
    uint64 e = (i == NCPU - 1)? PHYSTOP : base + (i + 1) * piece;
    initlock(&kmem[i].lock, "kmem");
    freerange((void*)s, (void*)e);
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  push_off();
  int cpu_id = cpuid();
  pop_off();
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu_id = cpuid();
  pop_off();

  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
    kmem[cpu_id].freelist = r->next;
  release(&kmem[cpu_id].lock);
  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    return (void*)r;
  }
  
  if(!r){
    for(int i = 0; i < NCPU; i++){
      if(cpu_id == i) continue;
      int flag = 0;
      for(int j = 0; j < 8; j++){
        struct run* p = 0;

        acquire(&kmem[i].lock);
        p = kmem[i].freelist;
        if(p)
          kmem[i].freelist = p->next;
        release(&kmem[i].lock);
        if(!p) break;

        acquire(&kmem[cpu_id].lock);
        p->next = kmem[cpu_id].freelist;
        kmem[cpu_id].freelist = p;
        release(&kmem[cpu_id].lock);
        flag++;
      }
      if(flag) break;
    }
  }
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
    kmem[cpu_id].freelist = r->next;
  release(&kmem[cpu_id].lock);
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
