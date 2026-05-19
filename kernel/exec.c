#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "defs.h"
#include "elf.h"

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_RANDOM 25

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);
static int loadseg_ext4(pde_t *, uint64, char *, uint, uint);

static int
exec_script(char *script, char *hdr, char **argv)
{
  struct script_exec {
    char interp[MAXPATH];
    char iarg[MAXPATH];
    char resolved[MAXPATH];
    char *nargv[MAXARG];
  } *se;
  int i = 2;
  int j = 0;
  int n = 0;
  int ret;
  int has_shebang = hdr[0] == '#' && hdr[1] == '!';

  se = (struct script_exec *)kalloc();
  if(se == 0)
    return -1;
  memset(se, 0, sizeof(*se));

  if(has_shebang){
    while(hdr[i] == ' ' || hdr[i] == '\t')
      i++;
    while(hdr[i] && hdr[i] != '\n' && hdr[i] != '\r' &&
          hdr[i] != ' ' && hdr[i] != '\t' && j < sizeof(se->interp) - 1)
      se->interp[j++] = hdr[i++];
    se->interp[j] = 0;
    if(se->interp[0] == 0){
      kfree(se);
      return -1;
    }

    while(hdr[i] == ' ' || hdr[i] == '\t')
      i++;
    j = 0;
    while(hdr[i] && hdr[i] != '\n' && hdr[i] != '\r' &&
          hdr[i] != ' ' && hdr[i] != '\t' && j < sizeof(se->iarg) - 1)
      se->iarg[j++] = hdr[i++];
    se->iarg[j] = 0;
  } else {
    safestrcpy(se->interp, "/bin/sh", sizeof(se->interp));
    safestrcpy(se->iarg, "sh", sizeof(se->iarg));
  }

  if((strncmp(se->interp, "/bin/sh", 7) == 0 && se->interp[7] == 0) ||
     (strncmp(se->interp, "/bin/busybox", 12) == 0 && se->interp[12] == 0) ||
     (strncmp(se->interp, "/busybox", 8) == 0 && se->interp[8] == 0)){
    struct proc *p = myproc();
    if(p->cwd_is_ext4){
      char *slash;
      safestrcpy(se->resolved, p->ext4_cwd, sizeof(se->resolved));
      for(;;){
        snprintf(se->interp, sizeof(se->interp), "%s/busybox", se->resolved);
        if(ext4_path_is_reg(FIRSTDEV, se->interp))
          break;
        slash = 0;
        for(char *q = se->resolved; *q; q++)
          if(*q == '/')
            slash = q;
        if(slash == 0 || slash == se->resolved){
          safestrcpy(se->interp, "/musl/busybox", sizeof(se->interp));
          break;
        }
        *slash = 0;
      }
    }
    if(se->iarg[0] == 0)
      safestrcpy(se->iarg, "sh", sizeof(se->iarg));
  }

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

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  struct exec_state {
    char epath[MAXPATH];
    char interp[MAXPATH];
    char interp_epath[MAXPATH];
    uint64 ustack[MAXARG];
    struct elfhdr elf;
    struct elfhdr ielf;
    struct proghdr ph;
  } *st;
  char *s, *last;
  int i, off;
  int is_ext4 = 0;
  int has_interp = 0;
  uint64 argc, sz = 0, sp, stackbase, phdr_addr = 0;
  uint64 app_brk, at_base = 0, entry;
  int stack_pages;
  struct inode *ip;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  int ret;

  st = (struct exec_state *)kalloc();
  if(st == 0)
    return -1;
  memset(st, 0, sizeof(*st));

  // Open the executable file.
  ip = 0;
  if(path[0] != '/' && p->cwd_is_ext4 &&
     resolve_ext4_path(path, st->epath, sizeof(st->epath)) &&
     ext4_path_is_reg(FIRSTDEV, st->epath)){
    is_ext4 = 1;
  } else {
    begin_op();
    if((ip = namei(path)) == 0){
      end_op();
      if(resolve_ext4_path(path, st->epath, sizeof(st->epath)) &&
         ext4_path_is_reg(FIRSTDEV, st->epath)){
        is_ext4 = 1;
      } else {
        kfree(st);
        return -1;
      }
    } else {
      ilock(ip);
    }
  }

  // Read the ELF header.
  if(is_ext4){
    if(ext4_read_file_by_path_at(FIRSTDEV, st->epath, (uchar *)&st->elf, sizeof(st->elf), 0) != sizeof(st->elf))
      goto bad;
  } else if(readi(ip, 0, (uint64)&st->elf, 0, sizeof(st->elf)) != sizeof(st->elf)){
    goto bad;
  }

  // Is this really an ELF file?
  if(st->elf.magic != ELF_MAGIC){
    char hdr[128];
    memset(hdr, 0, sizeof(hdr));
    if(is_ext4){
      ext4_read_file_by_path_at(FIRSTDEV, st->epath, (uchar *)hdr, sizeof(hdr) - 1, 0);
      ret = exec_script(st->epath, hdr, argv);
    } else {
      readi(ip, 0, (uint64)hdr, 0, sizeof(hdr) - 1);
      iunlockput(ip);
      end_op();
      ip = 0;
      ret = exec_script(path, hdr, argv);
    }
    kfree(st);
    return ret;
  }

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=st->elf.phoff; i<st->elf.phnum; i++, off+=sizeof(st->ph)){
    if(is_ext4){
      if(ext4_read_file_by_path_at(FIRSTDEV, st->epath, (uchar *)&st->ph, sizeof(st->ph), off) != sizeof(st->ph))
        goto bad;
    } else if(readi(ip, 0, (uint64)&st->ph, off, sizeof(st->ph)) != sizeof(st->ph)){
      goto bad;
    }
    if(st->ph.type == ELF_PROG_INTERP && is_ext4){
      if(st->ph.filesz == 0 || st->ph.filesz >= sizeof(st->interp))
        goto bad;
      if(ext4_read_file_by_path_at(FIRSTDEV, st->epath, (uchar *)st->interp, st->ph.filesz, st->ph.off) != st->ph.filesz)
        goto bad;
      st->interp[st->ph.filesz - 1] = 0;
      has_interp = 1;
    }
    if(st->ph.type != ELF_PROG_LOAD)
      continue;
    if(st->elf.phoff >= st->ph.off && st->elf.phoff < st->ph.off + st->ph.filesz)
      phdr_addr = st->ph.vaddr + (st->elf.phoff - st->ph.off);
    if(st->ph.memsz < st->ph.filesz)
      goto bad;
    if(st->ph.vaddr + st->ph.memsz < st->ph.vaddr)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, st->ph.vaddr + st->ph.memsz, flags2perm(st->ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(is_ext4){
      if(loadseg_ext4(pagetable, st->ph.vaddr, st->epath, st->ph.off, st->ph.filesz) < 0)
        goto bad;
    } else if(loadseg(pagetable, st->ph.vaddr, ip, st->ph.off, st->ph.filesz) < 0){
      goto bad;
    }
  }
  if(is_ext4 == 0){
    iunlockput(ip);
    end_op();
    ip = 0;
  }

  entry = st->elf.entry;
  app_brk = PGROUNDUP(sz);

  if(has_interp){
    uint64 base, sz1;

    if(resolve_ext4_path(st->interp, st->interp_epath, sizeof(st->interp_epath)) == 0 ||
       ext4_path_is_reg(FIRSTDEV, st->interp_epath) == 0)
      goto bad;
    if(ext4_read_file_by_path_at(FIRSTDEV, st->interp_epath, (uchar *)&st->ielf, sizeof(st->ielf), 0) != sizeof(st->ielf))
      goto bad;
    if(st->ielf.magic != ELF_MAGIC)
      goto bad;

    base = PGROUNDUP(sz) + 16 * PGSIZE;
    at_base = base;
    for(i = 0, off = st->ielf.phoff; i < st->ielf.phnum; i++, off += sizeof(st->ph)){
      if(ext4_read_file_by_path_at(FIRSTDEV, st->interp_epath, (uchar *)&st->ph, sizeof(st->ph), off) != sizeof(st->ph))
        goto bad;
      if(st->ph.type != ELF_PROG_LOAD)
        continue;
      if(st->ph.memsz < st->ph.filesz)
        goto bad;
      if(base + st->ph.vaddr + st->ph.memsz < base + st->ph.vaddr)
        goto bad;
      if((sz1 = uvmalloc(pagetable, sz, base + st->ph.vaddr + st->ph.memsz, flags2perm(st->ph.flags))) == 0)
        goto bad;
      sz = sz1;
      if(loadseg_ext4(pagetable, base + st->ph.vaddr, st->interp_epath, st->ph.off, st->ph.filesz) < 0)
        goto bad;
    }
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
    uint64 auxv[] = {
      AT_PHDR, phdr_addr,
      AT_PHENT, sizeof(struct proghdr),
      AT_PHNUM, st->elf.phnum,
      AT_PAGESZ, PGSIZE,
      AT_BASE, at_base,
      AT_FLAGS, 0,
      AT_ENTRY, st->elf.entry,
      AT_UID, 0,
      AT_EUID, 0,
      AT_GID, 0,
      AT_EGID, 0,
      AT_RANDOM, 0,
      AT_NULL, 0,
    };
    int auxbytes = sizeof(auxv);
    int total;

    sp -= sizeof(random);
    sp -= sp % 16;
    rand_addr = sp;
    auxv[23] = rand_addr;
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
    if(copyout(pagetable, auxv_addr, (char *)auxv, auxbytes) < 0)
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
  oldpagetable = p->pagetable;
  proc_munmapall(p);
  p->pagetable = pagetable;
  p->sz = sz;
  p->is_linux = is_ext4;
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
  if(ip && is_ext4 == 0){
    iunlockput(ip);
    end_op();
  }
  kfree(st);
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
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
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}

static int
loadseg_ext4(pagetable_t pagetable, uint64 va, char *path, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += n){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg_ext4: address should exist");
    n = PGSIZE - ((va + i) % PGSIZE);
    if(sz - i < n)
      n = sz - i;
    if(ext4_read_file_by_path_at(FIRSTDEV, path, (uchar *)pa, n, offset+i) != n)
      return -1;
  }

  return 0;
}
