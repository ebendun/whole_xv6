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
} kmem;

struct superrun {
  struct superrun *next;
};

struct {
  struct spinlock lock;
  struct superrun *freelist;
} supermem;

static uint64 super_start;
static uint64 super_end;
static uint refcnt[PHYSTOP / PGSIZE];
static uint super_refcnt[PHYSTOP / SUPERPGSIZE];

static inline uint
pa2ref(uint64 pa)
{
  return pa / PGSIZE;
}

static inline uint
pa2super(uint64 pa)
{
  return (pa - super_start) / SUPERPGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&supermem.lock, "supermem");
  super_end = PHYSTOP;
  super_start = PHYSTOP - 16 * SUPERPGSIZE;
  super_start = SUPERPGROUNDDOWN(super_start);

  for(char *p = (char*)super_start; p + SUPERPGSIZE <= (char*)super_end; p += SUPERPGSIZE)
  {
    acquire(&supermem.lock);
    super_refcnt[pa2super((uint64)p)] = 1;
    release(&supermem.lock);
    superfree(p);
  }
  freerange(end, (void*)super_start);

}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&kmem.lock);
    refcnt[pa2ref((uint64)p)] = 1;
    release(&kmem.lock);
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int freeit = 0;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  if(refcnt[pa2ref((uint64)pa)] < 1)
    panic("kfree ref");
  refcnt[pa2ref((uint64)pa)]--;
  if(refcnt[pa2ref((uint64)pa)] == 0)
    freeit = 1;
  release(&kmem.lock);

  if(!freeit)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void
superfree(void *pa)
{
  struct superrun *r;
  int freeit = 0;

  if(((uint64)pa % SUPERPGSIZE) != 0 || (uint64)pa < super_start ||
     (uint64)pa + SUPERPGSIZE > super_end)
    panic("superfree");

  acquire(&supermem.lock);
  if(super_refcnt[pa2super((uint64)pa)] < 1)
    panic("superfree ref");
  super_refcnt[pa2super((uint64)pa)]--;
  if(super_refcnt[pa2super((uint64)pa)] == 0)
    freeit = 1;
  release(&supermem.lock);

  if(!freeit)
    return;

  memset(pa, 1, SUPERPGSIZE);

  r = (struct superrun*)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}

void *
superalloc(void)
{
  struct superrun *r;

  acquire(&supermem.lock);
  r = supermem.freelist;
  if(r)
    supermem.freelist = r->next;
  if(r)
    super_refcnt[pa2super((uint64)r)] = 1;
  release(&supermem.lock);

  if(r)
    memset((char*)r, 5, SUPERPGSIZE);
  return (void*)r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  if(r)
    refcnt[pa2ref((uint64)r)] = 1;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void
kref_inc(uint64 pa)
{
  if((pa % PGSIZE) != 0 || pa >= PHYSTOP)
    panic("kref_inc");

  acquire(&kmem.lock);
  refcnt[pa2ref(pa)]++;
  release(&kmem.lock);
}

uint
kref_get(uint64 pa)
{
  uint n;
  if((pa % PGSIZE) != 0 || pa >= PHYSTOP)
    panic("kref_get");

  acquire(&kmem.lock);
  n = refcnt[pa2ref(pa)];
  release(&kmem.lock);
  return n;
}

void
superref_inc(uint64 pa)
{
  if((pa % SUPERPGSIZE) != 0 || pa < super_start || pa + SUPERPGSIZE > super_end)
    panic("superref_inc");

  acquire(&supermem.lock);
  super_refcnt[pa2super(pa)]++;
  release(&supermem.lock);
}

uint
superref_get(uint64 pa)
{
  uint n;
  if((pa % SUPERPGSIZE) != 0 || pa < super_start || pa + SUPERPGSIZE > super_end)
    panic("superref_get");

  acquire(&supermem.lock);
  n = super_refcnt[pa2super(pa)];
  release(&supermem.lock);
  return n;
}
