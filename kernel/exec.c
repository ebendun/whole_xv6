#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "defs.h"
#include "elf.h"
#include "vfs.h"
#include "exec.h"

static int loadseg_file(pde_t *, uint64, struct file *, uint, uint);
static int load_elf_load_segments(pagetable_t, struct file *, struct elfhdr *,
                                  struct proghdr *, uint64, uint64 *,
                                  uint64 *);
static void build_auxv(uint64 *, uint64, struct elfhdr *, uint64, uint64);
extern struct proc proc[NPROC];

// Resolve a process-relative path through VFS and open it as a regular file.
// On success, vp records the resolved VFS location and *fp is the opened file.
static int
exec_open_vfs(char *path, struct vfs_path *vp, struct file **fp)
{
  struct proc *p = myproc();
  struct vfs_node node;

  if(vfs_resolve_proc_path(p, path, vp) < 0)
    return -1;
  if(vp->ops == 0 || vp->ops->inode == 0 || vp->ops->inode->lookup == 0 ||
     vp->ops->file == 0 || vp->ops->file->open == 0)
    return -1;
  if(vp->ops->inode->lookup(vp->mount, vp->inner, &node) < 0 ||
     node.type != T_FILE)
    return -1;
  return vp->ops->file->open(vp->mount, vp->inner, 0, fp);
}

// Open the dynamic linker named by PT_INTERP.  Linux binaries from ext4 may
// name /lib/..., which we redirect into the mounted glibc or musl trees.
static int
exec_open_interp(char *path, int from_ext4, struct vfs_path *vp, struct file **fp)
{
  char full[MAXPATH];
  struct proc *p = myproc();
  int rooted_ext4 = p->vfs_root.mount && p->vfs_root.mount->type == VFS_EXT4;

  if(from_ext4 && path[0] == '/'){
    if(strncmp(path, "/lib/", 5) == 0){
      snprintf(full, sizeof(full), rooted_ext4 ? "/glibc%s" : "/ext4/glibc%s", path);
      if(exec_open_vfs(full, vp, fp) == 0)
        return 0;
      snprintf(full, sizeof(full), rooted_ext4 ? "/musl%s" : "/ext4/musl%s", path);
      if(exec_open_vfs(full, vp, fp) == 0)
        return 0;
    }
    snprintf(full, sizeof(full), rooted_ext4 ? "%s" : "/ext4%s", path);
    return exec_open_vfs(full, vp, fp);
  }
  return exec_open_vfs(path, vp, fp);
}

// Non-ELF files are treated as shell scripts handled by busybox sh.
// This keeps script handling in user space instead of teaching the kernel
// about every possible shebang interpreter.
static int
exec_nonelf(char *script, char **argv)
{
  struct script_exec *se;
  int i;
  int n = 0;
  int ret;
  struct proc *p = myproc();

  se = (struct script_exec *)kalloc();
  if(se == 0)
    return -1;
  memset(se, 0, sizeof(*se));

  if(p->vfs_root.mount && p->vfs_root.mount->type == VFS_EXT4)
    safestrcpy(se->interp, "/musl/busybox", sizeof(se->interp));
  else
    safestrcpy(se->interp, "/ext4/musl/busybox", sizeof(se->interp));
  safestrcpy(se->iarg, "sh", sizeof(se->iarg));

  se->nargv[n++] = se->interp;
  if(se->iarg[0])
    se->nargv[n++] = se->iarg;
  se->nargv[n++] = script;
  for(i = 1; argv[i] && n < MAXARG - 1; i++)
    se->nargv[n++] = argv[i];
  se->nargv[n] = 0;

  ret = kexec(se->interp, se->nargv);
  kfree(se);
  return ret;
}

// Map ELF segment permissions to the small subset of PTE bits we use here.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

// Replace the current process image with path.  Most work happens in a fresh
// pagetable first; the old image is discarded only after every load step works.
int
kexec(char *path, char **argv)
{
  struct exec_state *st;
  char *s, *last;
  int i, off;
  int is_ext4 = 0;
  int has_interp = 0;
  uint64 argc, sz = 0, sp, stackbase, phdr_addr = 0;
  uint64 app_brk, at_base = 0, entry;
  int stack_pages;
  struct file *ef = 0;
  struct file *interp_f = 0;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  int ret;

  st = (struct exec_state *)kalloc();
  if(st == 0)
    return -1;
  memset(st, 0, sizeof(*st));

  if(exec_open_vfs(path, &st->vp, &ef) < 0){
    kfree(st);
    return -1;
  }
  safestrcpy(st->epath, st->vp.type == VFS_EXT4 ? st->vp.inner : st->vp.abs_path,
             sizeof(st->epath));
  is_ext4 = st->vp.type == VFS_EXT4;

  // acquire the ELF header.
  if(vfs_file_read_kernel(ef, (char *)&st->elf, sizeof(st->elf), 0) != sizeof(st->elf))
    goto bad;

  // Is this really an ELF file?
  if(st->elf.magic != ELF_MAGIC){
    fileclose(ef);
    ef = 0;
    ret = exec_nonelf(path, argv);
    kfree(st);
    return ret;
  }

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory and find an optional Linux interpreter.
  for(i=0, off=st->elf.phoff; i<st->elf.phnum; i++, off+=sizeof(st->ph)){
    if(vfs_file_read_kernel(ef, (char *)&st->ph, sizeof(st->ph), off) != sizeof(st->ph))
      goto bad;
    if(st->ph.type == ELF_PROG_INTERP && is_ext4){
      if(st->ph.filesz == 0 || st->ph.filesz >= sizeof(st->interp))
        goto bad;
      if(vfs_file_read_kernel(ef, st->interp, st->ph.filesz, st->ph.off) != st->ph.filesz)
        goto bad;
      st->interp[st->ph.filesz - 1] = 0;
      has_interp = 1;
    }
  }
  if(load_elf_load_segments(pagetable, ef, &st->elf, &st->ph, 0, &sz,
                            &phdr_addr) < 0)
    goto bad;
  fileclose(ef);
  ef = 0;

  entry = st->elf.entry;
  app_brk = PGROUNDUP(sz);

  if(has_interp){
    uint64 base;

    if(exec_open_interp(st->interp, is_ext4, &st->vp, &interp_f) < 0)
      goto bad;
    safestrcpy(st->interp_epath, st->vp.type == VFS_EXT4 ? st->vp.inner : st->vp.abs_path,
               sizeof(st->interp_epath));
    if(vfs_file_read_kernel(interp_f, (char *)&st->ielf, sizeof(st->ielf), 0) != sizeof(st->ielf))
      goto bad;
    if(st->ielf.magic != ELF_MAGIC)
      goto bad;

    base = PGROUNDUP(sz) + 16 * PGSIZE;
    at_base = base;
    if(load_elf_load_segments(pagetable, interp_f, &st->ielf, &st->ph, base,
                              &sz, 0) < 0)
      goto bad;
    fileclose(interp_f);
    interp_f = 0;
    entry = base + st->ielf.entry;
  }

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  if(is_ext4)
    p->linux_brk = app_brk;
  if(is_ext4)
    sz += has_interp ? 16 * PGSIZE : 4096 * PGSIZE;
  if(is_ext4)
    p->linux_brk_limit = has_interp ? at_base : sz;
  stack_pages = is_ext4 ? 16 : USERSTACK;
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + (stack_pages+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(stack_pages+1)*PGSIZE);
  sp = sz;
  stackbase = sp - stack_pages*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    st->ustack[argc] = sp;
  }
  st->ustack[argc] = 0;

  {
    uint64 rand_addr, argv_addr, envp_addr, auxv_addr;
    uint64 random[2] = {0, 0};
    int auxbytes = sizeof(st->auxv);
    int total;

    sp -= sizeof(random);
    sp -= sp % 16;
    rand_addr = sp;
    build_auxv(st->auxv, phdr_addr, &st->elf, at_base, rand_addr);
    if(sp < stackbase || copyout(pagetable, rand_addr, (char *)random, sizeof(random)) < 0)
      goto bad;

    total = sizeof(uint64) + (argc + 1) * sizeof(uint64) +
            sizeof(uint64) + auxbytes;
    sp -= total;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;

    argv_addr = sp + sizeof(uint64);
    envp_addr = argv_addr + (argc + 1) * sizeof(uint64);
    auxv_addr = envp_addr + sizeof(uint64);

    if(copyout(pagetable, sp, (char *)&argc, sizeof(argc)) < 0)
      goto bad;
    if(copyout(pagetable, argv_addr, (char *)st->ustack, (argc+1)*sizeof(uint64)) < 0)
      goto bad;
    uint64 zero = 0;
    if(copyout(pagetable, envp_addr, (char *)&zero, sizeof(zero)) < 0)
      goto bad;
    if(copyout(pagetable, auxv_addr, (char *)st->auxv, auxbytes) < 0)
      goto bad;

    // xv6 user programs enter main directly; Linux ABI programs read from sp.
    p->trapframe->a1 = argv_addr;
    p->trapframe->a2 = envp_addr;
    p->trapframe->ra = 0;
  }

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  if(p->is_linux){
    for(struct proc *q = proc; q < &proc[NPROC]; q++){
      if(q == p || q->state == UNUSED || linux_tgid(q) != linux_tgid(p))
        continue;
      acquire(&q->lock);
      q->killed = 1;
      if(q->state == SLEEPING)
        q->state = RUNNABLE;
      release(&q->lock);
    }
    p->linux_is_thread = 0;
    p->linux_group_leader = p;
    p->linux_tgid = p->pid;
    p->linux_thread_count = 1;
    p->linux_group_exiting = 0;
    p->linux_group_xstate = 0;
  }
  oldpagetable = p->pagetable;
  proc_munmapall(p);
  p->pagetable = pagetable;
  p->sz = sz;
  p->is_linux = is_ext4;
  if(is_ext4){
    vfs_set_proc_root(p, "/ext4");
    p->vfs_redirect = 1;
    safestrcpy(p->vfs_redirect_root, "/tmp", sizeof(p->vfs_redirect_root));
  } else {
    vfs_set_proc_root(p, "/");
    p->vfs_redirect = 0;
    p->vfs_redirect_root[0] = 0;
  }
  p->mmap_base = USIGRETURN;
  p->trapframe->epc = entry;  // initial program counter
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  // xv6 programs enter main(argc, argv). Linux ABI programs enter _start;
  // on RISC-V a0 is rtld_fini, so keep it null and let _start read argc
  // from the initial stack.
  ret = is_ext4 ? 0 : argc;
  kfree(st);
  return ret;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ef)
    fileclose(ef);
  if(interp_f)
    fileclose(interp_f);
  kfree(st);
  return -1;
}

// Load every PT_LOAD segment from an ELF file.  base is zero for the main
// program and nonzero when placing a dynamic linker above the program image.
static int
load_elf_load_segments(pagetable_t pagetable, struct file *f,
                       struct elfhdr *elf, struct proghdr *ph, uint64 base,
                       uint64 *sz, uint64 *phdr_addr)
{
  int i, off;

  for(i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(*ph)){
    if(vfs_file_read_kernel(f, (char *)ph, sizeof(*ph), off) != sizeof(*ph))
      return -1;
    if(ph->type != ELF_PROG_LOAD)
      continue;
    if(phdr_addr && elf->phoff >= ph->off && elf->phoff < ph->off + ph->filesz)
      *phdr_addr = base + ph->vaddr + (elf->phoff - ph->off);
    if(ph->memsz < ph->filesz)
      return -1;
    if(base + ph->vaddr + ph->memsz < base + ph->vaddr)
      return -1;

    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, *sz, base + ph->vaddr + ph->memsz,
                       flags2perm(ph->flags))) == 0)
      return -1;
    *sz = sz1;
    if(loadseg_file(pagetable, base + ph->vaddr, f, ph->off, ph->filesz) < 0)
      return -1;
  }

  return 0;
}

// Build the auxiliary vector expected by Linux-style startup code and rtld.
// xv6 programs ignore most of it, but ext4/Linux binaries need these entries.
static void
build_auxv(uint64 *auxv, uint64 phdr_addr, struct elfhdr *elf, uint64 at_base,
           uint64 rand_addr)
{
  auxv[0] = AT_PHDR;   auxv[1] = phdr_addr;
  auxv[2] = AT_PHENT;  auxv[3] = sizeof(struct proghdr);
  auxv[4] = AT_PHNUM;  auxv[5] = elf->phnum;
  auxv[6] = AT_PAGESZ; auxv[7] = PGSIZE;
  auxv[8] = AT_BASE;   auxv[9] = at_base;
  auxv[10] = AT_FLAGS; auxv[11] = 0;
  auxv[12] = AT_ENTRY; auxv[13] = elf->entry;
  auxv[14] = AT_UID;   auxv[15] = 0;
  auxv[16] = AT_EUID;  auxv[17] = 0;
  auxv[18] = AT_GID;   auxv[19] = 0;
  auxv[20] = AT_EGID;  auxv[21] = 0;
  auxv[22] = AT_RANDOM; auxv[23] = rand_addr;
  auxv[24] = AT_NULL;  auxv[25] = 0;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg_file(pagetable_t pagetable, uint64 va, struct file *f, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += n){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    n = PGSIZE - ((va + i) % PGSIZE);
    if(sz - i < n)
      n = sz - i;
    if(vfs_file_read_kernel(f, (char *)pa, n, offset+i) != n)
      return -1;
  }

  return 0;
}
