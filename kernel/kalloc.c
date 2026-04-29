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

  for(int i = 0; i < NCPU; i++){
    uint64 base = PGROUNDUP((uint64)end);
    uint64 total_mem = super_start - base;
    uint64 piece = total_mem / NCPU;
    uint64 s = base + i * piece;
    uint64 e = (i == NCPU - 1)? super_start : base + (i + 1) * piece;
    initlock(&kmem[i].lock, "kmem");
    freerange((void*)s, (void*)e);
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  push_off();
  int cpu_id = cpuid();
  pop_off();
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&kmem[cpu_id].lock);
    refcnt[pa2ref((uint64)p)] = 1;
    release(&kmem[cpu_id].lock);
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

  push_off();
  int cpu_id = cpuid();
  pop_off();
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem[cpu_id].lock);
  if(refcnt[pa2ref((uint64)pa)] < 1)
    panic("kfree ref");
  refcnt[pa2ref((uint64)pa)]--;
  if(refcnt[pa2ref((uint64)pa)] == 0)
    freeit = 1;
  release(&kmem[cpu_id].lock);

  if(!freeit)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
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

  
  push_off();
  int cpu_id = cpuid();
  pop_off();
  
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
  kmem[cpu_id].freelist = r->next;
  if(r)
    refcnt[pa2ref((uint64)r)] = 1;
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
  if(r)
    refcnt[pa2ref((uint64)r)] = 1;
  release(&kmem[cpu_id].lock);
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void
kref_inc(uint64 pa)
{
  if((pa % PGSIZE) != 0 || pa >= PHYSTOP)
    panic("kref_inc");
  push_off();
  int cpu_id = cpuid();
  pop_off();
  acquire(&kmem[cpu_id].lock);
  refcnt[pa2ref(pa)]++;
  release(&kmem[cpu_id].lock);
}

uint
kref_get(uint64 pa)
{
  uint n;
  if((pa % PGSIZE) != 0 || pa >= PHYSTOP)
    panic("kref_get");

  push_off();
  int cpu_id = cpuid();
  pop_off();
  acquire(&kmem[cpu_id].lock);
  n = refcnt[pa2ref(pa)];
  release(&kmem[cpu_id].lock);
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
