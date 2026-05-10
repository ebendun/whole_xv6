#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // ext4  virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  
  // init xv6 fs virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);

  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
      if(PTE_LEAF(*pte)) {
        return pte;
      }
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

static pte_t *
walk_to_level(pagetable_t pagetable, uint64 va, int target_level, int alloc)
{
  if(va >= MAXVA)
    panic("walk_to_level");

  for(int level = 2; level > target_level; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      if(PTE_LEAF(*pte))
        return pte;
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc)
        return 0;
      if((pagetable = (pagetable_t)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(target_level, va)];
}

static pte_t *
walk_with_level(pagetable_t pagetable, uint64 va, int alloc, int *level_out)
{
  if(va >= MAXVA)
    panic("walk_with_level");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      if(PTE_LEAF(*pte)) {
        if(level_out)
          *level_out = level;
        return pte;
      }
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc)
        return 0;
      if((pagetable = (pagetable_t)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  if(level_out)
    *level_out = 0;
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;
  int level = 0;

  if(va >= MAXVA)
    return 0;
  pte = walk_with_level(pagetable, va, 0, &level);

  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  if(level == 1)
    pa += va & (SUPERPGSIZE - 1);
  else
    pa += va & (PGSIZE - 1);

  return pa;
}

void
vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  int start_level = 2;

  for(int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    uint64 va = ((uint64)i << PXSHIFT(start_level));
    for(int d = 0; d < 1; d++)
      printf(" ..");
    printf("%p: pte %p pa %p\n", (void*)va, (void*)pte, (void*)PTE2PA(pte));
    if((pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      pagetable_t child = (pagetable_t)PTE2PA(pte);
      for(int j = 0; j < 512; j++) {
        pte_t pte1 = child[j];
        if((pte1 & PTE_V) == 0)
          continue;
        uint64 va1 = va | ((uint64)j << PXSHIFT(start_level - 1));
        for(int d = 0; d < 2; d++)
          printf(" ..");
        printf("%p: pte %p pa %p\n", (void*)va1, (void*)pte1, (void*)PTE2PA(pte1));
        if((pte1 & (PTE_R|PTE_W|PTE_X)) == 0) {
          pagetable_t child2 = (pagetable_t)PTE2PA(pte1);
          for(int k = 0; k < 512; k++) {
            pte_t pte2 = child2[k];
            if((pte2 & PTE_V) == 0)
              continue;
            uint64 va2 = va1 | ((uint64)k << PXSHIFT(start_level - 2));
            for(int d = 0; d < 3; d++)
              printf(" ..");
            printf("%p: pte %p pa %p\n", (void*)va2, (void*)pte2, (void*)PTE2PA(pte2));
          }
        }
      }
    }
  }
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  int sz = PGSIZE;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += sz){
    int level = 0;
    pte = walk_with_level(pagetable, a, 0, &level);

    if(pte == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;

    if(level == 1 && PTE_LEAF(*pte)) {
      uint64 remaining = va + npages*PGSIZE - a;
      if((a % SUPERPGSIZE) == 0 && remaining >= SUPERPGSIZE) {
        sz = SUPERPGSIZE;
        if(do_free)
          superfree((void*)PTE2PA(*pte));
        *pte = 0;
        continue;
      }
      // Demote superpage to 4K pages for partial unmap.
      uint64 super_pa = PTE2PA(*pte);
      pagetable_t newpt = uvmcreate();
      if(newpt == 0)
        panic("uvmunmap: demote alloc");
      memset(newpt, 0, PGSIZE);
      uint flags = PTE_FLAGS(*pte);
      for(int i = 0; i < 512; i++) {
        char *mem = kalloc();
        if(mem == 0)
          panic("uvmunmap: demote page");
        memmove(mem, (void*)(super_pa + i * PGSIZE), PGSIZE);
        newpt[i] = PA2PTE(mem) | (flags | PTE_V);
      }
      superfree((void*)super_pa);
      *pte = PA2PTE(newpt) | PTE_V;
      pte = walk(pagetable, a, 0);
      if(pte == 0)
        panic("uvmunmap: demote walk");
      sz = PGSIZE;
    }

    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
    sz = PGSIZE;
  }
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += sz){
    sz = PGSIZE;
    if((a % SUPERPGSIZE) == 0 && newsz - a >= SUPERPGSIZE) {
      mem = superalloc();
      if(mem != 0){
        memset(mem, 0, SUPERPGSIZE);
        pte_t *pte = walk_to_level(pagetable, a, 1, 1);
        if(pte == 0) {
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        if((*pte & PTE_V) == 0) {
          *pte = PA2PTE(mem) | PTE_R|PTE_U|xperm | PTE_V;
          sz = SUPERPGSIZE;
          continue;
        }
        // Existing page-table page prevents superpage mapping; fall back to 4K.
        superfree(mem);
      }
    }
    // Fall back to 4K pages when superpage pool is exhausted.
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, sz);
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  int szinc = PGSIZE;

  for(i = 0; i < sz; i += szinc){
    int level = 0;
    if((pte = walk_with_level(old, i, 0, &level)) == 0)
      continue;

    if((*pte & PTE_V) == 0)
      continue;

    if(level == 1 && PTE_LEAF(*pte)) {
      if((i % SUPERPGSIZE) != 0)
        panic("uvmcopy: unaligned superpage");
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if(flags & PTE_W){
        flags = (flags & ~PTE_W) | PTE_COW;
        *pte = PA2PTE(pa) | flags;
      }
      pte_t *npte = walk_to_level(new, i, 1, 1);
      if(npte == 0 || (*npte & PTE_V)) {
        goto err;
      }
      *npte = PA2PTE(pa) | flags | PTE_V;
      superref_inc(pa);
      szinc = SUPERPGSIZE;
      continue;
    }

    szinc = PGSIZE;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if(flags & PTE_W){
      flags = (flags & ~PTE_W) | PTE_COW;
      *pte = PA2PTE(pa) | flags;
    }
    if(mappages(new, i, PGSIZE, pa, flags) != 0)
      goto err;
    kref_inc(pa);
  }
  sfence_vma();
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;
  struct proc *p = myproc();
  uint64 psz;

  if(p == 0)
    return -1;

  psz = (pagetable == p->pagetable) ? p->sz : MAXVA;
  if(dstva >= psz || len > psz - dstva)
    return -1;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 1)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    if((*pte & PTE_COW) != 0){
      if((pa0 = vmfault(pagetable, va0, 0)) == 0)
        return -1;
      pte = walk(pagetable, va0, 0);
    }
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();
  uint64 psz;

  if(p == 0)
    return -1;

  psz = (pagetable == p->pagetable) ? p->sz : MAXVA;
  if(srcva >= psz || len > psz - srcva)
    return -1;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;
  struct proc *p = myproc();
  uint64 psz;

  if(p == 0)
    return -1;

  psz = (pagetable == p->pagetable) ? p->sz : MAXVA;
  if(srcva >= psz)
    return -1;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    if(va0 >= psz)
      return -1;
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  uint64 pa;
  pte_t *pte;
  int level = 0;
  struct proc *p = myproc();
  
  struct vma *v = 0;
  uint64 a = PGROUNDDOWN(va);

  if(va >= MAXVA || a >= MAXVA)
    return 0;
  va = PGROUNDDOWN(va);
  pte = walk_with_level(pagetable, va, 0, &level);
  if(pte && (*pte & PTE_V)){
    if(((*pte & PTE_COW) != 0) && !read){
      pa = PTE2PA(*pte);
      if(level == 1 && PTE_LEAF(*pte)){
        uint64 off = va & (SUPERPGSIZE - 1);
        uint flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
        if(superref_get(pa) == 1){
          *pte = PA2PTE(pa) | flags;
          sfence_vma();
          return pa + off;
        }
        mem = (uint64)superalloc();
        if(mem != 0){
          memmove((void *)mem, (void *)pa, SUPERPGSIZE);
          *pte = PA2PTE(mem) | flags;
          sfence_vma();
          superfree((void *)pa);
          return mem + off;
        }

        // If no free 2MB page exists, split into 4KB private pages.
        pagetable_t newpt = (pagetable_t)kalloc();
        if(newpt == 0)
          return 0;
        memset(newpt, 0, PGSIZE);
        for(int i = 0; i < 512; i++){
          char *pg = kalloc();
          if(pg == 0){
            for(int j = 0; j < i; j++){
              if(newpt[j] & PTE_V)
                kfree((void*)PTE2PA(newpt[j]));
            }
            kfree((void*)newpt);
            return 0;
          }
          memmove(pg, (void *)(pa + i * PGSIZE), PGSIZE);
          newpt[i] = PA2PTE(pg) | flags | PTE_V;
        }
        *pte = PA2PTE(newpt) | PTE_V;
        sfence_vma();
        superfree((void *)pa);
        pte_t *lpte = walk(pagetable, va, 0);
        if(lpte == 0 || (*lpte & PTE_V) == 0)
          return 0;
        return PTE2PA(*lpte) + (va & (PGSIZE - 1));
      }

      if(kref_get(pa) == 1){
        uint flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
        *pte = PA2PTE(pa) | flags;
        sfence_vma();
        return pa;
      }
      mem = (uint64)kalloc();
      if(mem == 0)
        return 0;
      memmove((void *)mem, (void *)pa, PGSIZE);
      uint flags = PTE_FLAGS(*pte);
      flags = (flags | PTE_W) & ~PTE_COW;
      *pte = PA2PTE(mem) | flags;
      sfence_vma();
      kfree((void *)pa);
      return mem;
    }
    return 0;
  }
  if(ismapped(pagetable, a)) {
    return 0;
  }

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0)
      continue;
    if(a >= p->vmas[i].addr && a < p->vmas[i].addr + p->vmas[i].len){
      v = &p->vmas[i];
      break;
    }
  }

  mem = (uint64)kalloc();
  if(mem == 0)
    return 0;
  memset((void*)mem, 0, PGSIZE);

  if(v){
    if(read && (v->prot & PROT_READ) == 0){
      kfree((void*)mem);
      return 0;
    }
    if(!read && (v->prot & PROT_WRITE) == 0){
      kfree((void*)mem);
      return 0;
    }

    uint64 off = v->offset + (a - v->addr);
    ilock(v->f->ip);
    int n = readi(v->f->ip, 0, mem, off, PGSIZE);
    iunlock(v->f->ip);
    if(n < 0){
      kfree((void*)mem);
      return 0;
    }

    int perm = PTE_U;
    if(v->prot & PROT_READ)
      perm |= PTE_R;
    if(v->prot & PROT_WRITE)
      perm |= PTE_W;
    if(v->prot & PROT_EXEC)
      perm |= PTE_X;

    if(mappages(p->pagetable, a, PGSIZE, mem, perm) != 0){
      kfree((void*)mem);
      return 0;
    }
    return mem;
  }

  if(a >= p->sz){
    kfree((void*)mem);
    return 0;
  }

  if(mappages(p->pagetable, a, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void*)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
