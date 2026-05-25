#ifndef XV6_EXEC_H
#define XV6_EXEC_H

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

#define EXEC_AUXV_LEN 26

struct script_exec {
  char interp[MAXPATH];
  char iarg[MAXPATH];
  char *nargv[MAXARG];
};

struct exec_state {
  char epath[MAXPATH];
  char interp[MAXPATH];
  char interp_epath[MAXPATH];
  uint64 ustack[MAXARG];
  struct elfhdr elf;
  struct elfhdr ielf;
  struct proghdr ph;
  struct vfs_path vp;
  uint64 auxv[EXEC_AUXV_LEN];
};

#endif
