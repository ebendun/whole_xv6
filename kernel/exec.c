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
  printf("exec_script\n");
  char interp[MAXPATH];
  char iarg[MAXPATH];
  char *nargv[MAXARG];
  int i = 2;
  int j = 0;
  int n = 0;

  if(hdr[0] != '#' || hdr[1] != '!')
    return -1;
  while(hdr[i] == ' ' || hdr[i] == '\t')
    i++;
  while(hdr[i] && hdr[i] != '\n' && hdr[i] != '\r' &&
        hdr[i] != ' ' && hdr[i] != '\t' && j < sizeof(interp) - 1)
    interp[j++] = hdr[i++];
  interp[j] = 0;
  if(interp[0] == 0)
    return -1;

  while(hdr[i] == ' ' || hdr[i] == '\t')
    i++;
  j = 0;
  while(hdr[i] && hdr[i] != '\n' && hdr[i] != '\r' &&
        hdr[i] != ' ' && hdr[i] != '\t' && j < sizeof(iarg) - 1)
    iarg[j++] = hdr[i++];
  iarg[j] = 0;

  nargv[n++] = interp;
  if(iarg[0])
    nargv[n++] = iarg;
  nargv[n++] = script;
  for(i = 1; argv[i] && n < MAXARG - 1; i++)
    nargv[n++] = argv[i];
  nargv[n] = 0;

  return kexec(interp, nargv);
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
  char *s, *last;
  int i, off;
  int is_ext4 = 0;
  char epath[MAXPATH];
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase, phdr_addr = 0;
  int stack_pages;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  // Open the executable file.
  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    if(resolve_ext4_path(path, epath, sizeof(epath)) &&
       ext4_path_is_reg(FIRSTDEV, epath)){
      is_ext4 = 1;
    } else {
      return -1;
    }
  } else {
    ilock(ip);
  }

  // Read the ELF header.
  if(is_ext4){
    if(ext4_read_file_by_path_at(FIRSTDEV, epath, (uchar *)&elf, sizeof(elf), 0) != sizeof(elf))
      goto bad;
  } else if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)){
    goto bad;
  }

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC){
    char hdr[128];
    int ret;
    memset(hdr, 0, sizeof(hdr));
    if(is_ext4){
      ext4_read_file_by_path_at(FIRSTDEV, epath, (uchar *)hdr, sizeof(hdr) - 1, 0);
      ret = exec_script(epath, hdr, argv);
    } else {
      readi(ip, 0, (uint64)hdr, 0, sizeof(hdr) - 1);
      iunlockput(ip);
      end_op();
      ip = 0;
      ret = exec_script(path, hdr, argv);
    }
    return ret;
  }

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(is_ext4){
      if(ext4_read_file_by_path_at(FIRSTDEV, epath, (uchar *)&ph, sizeof(ph), off) != sizeof(ph))
        goto bad;
    } else if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)){
      goto bad;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(elf.phoff >= ph.off && elf.phoff < ph.off + ph.filesz)
      phdr_addr = ph.vaddr + (elf.phoff - ph.off);
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(is_ext4){
      if(loadseg_ext4(pagetable, ph.vaddr, epath, ph.off, ph.filesz) < 0)
        goto bad;
    } else if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0){
      goto bad;
    }
  }
  if(is_ext4 == 0){
    iunlockput(ip);
    end_op();
    ip = 0;
  }

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  if(is_ext4)
    p->linux_brk = sz;
  if(is_ext4)
    sz += 4096 * PGSIZE;
  if(is_ext4)
    p->linux_brk_limit = sz;
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
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  {
    uint64 rand_addr, argv_addr, envp_addr, auxv_addr;
    uint64 random[2] = {0, 0};
    uint64 auxv[] = {
      AT_PHDR, phdr_addr,
      AT_PHENT, sizeof(struct proghdr),
      AT_PHNUM, elf.phnum,
      AT_PAGESZ, PGSIZE,
      AT_BASE, 0,
      AT_FLAGS, 0,
      AT_ENTRY, elf.entry,
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
    if(copyout(pagetable, argv_addr, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
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
  p->pagetable = pagetable;
  p->sz = sz;
  p->is_linux = is_ext4;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  // xv6 programs enter main(argc, argv). Linux ABI programs enter _start;
  // on RISC-V a0 is rtld_fini, so keep it null and let _start read argc
  // from the initial stack.
  return is_ext4 ? 0 : argc;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip && is_ext4 == 0){
    iunlockput(ip);
    end_op();
  }
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
