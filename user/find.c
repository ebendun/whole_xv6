#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

static char *
base_name(char *path)
{
  char *p;

  for(p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  return p + 1;
}

static void
run_exec(char *path, char **cmdv, int cmdc)
{
  char *argv[MAXARG];
  int i;
  int pid;

  if(cmdc + 2 > MAXARG){
    fprintf(2, "find: too many -exec args\n");
    return;
  }

  for(i = 0; i < cmdc; i++)
    argv[i] = cmdv[i];
  argv[cmdc] = path;
  argv[cmdc + 1] = 0;

  pid = fork();
  if(pid < 0){
    fprintf(2, "find: fork failed\n");
    return;
  }
  if(pid == 0){
    exec(argv[0], argv);
    fprintf(2, "find: exec %s failed\n", argv[0]);
    exit(1);
  }

  wait(0);
}

static void
find(char *path, char *name, char **cmdv, int cmdc)
{
  char buf[512], *p;
  int fd;
  struct stat st;
  struct dirent de;

  fd = open(path, O_RDONLY);
  if(fd < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if(strcmp(base_name(path), name) == 0){
    if(cmdc == 0)
      printf("%s\n", path);
    else
      run_exec(path, cmdv, cmdc);
  }

  if(st.type != T_DIR){
    close(fd);
    return;
  }

  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
    fprintf(2, "find: path too long\n");
    close(fd);
    return;
  }

  strcpy(buf, path);
  p = buf + strlen(buf);
  *p++ = '/';

  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = '\0';
    find(buf, name, cmdv, cmdc);
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "Usage: find path filename [-exec cmd ...]\n");
    exit(1);
  }

  if(argc == 3){
    find(argv[1], argv[2], 0, 0);
    exit(0);
  }

  if(strcmp(argv[3], "-exec") != 0 || argc < 5){
    fprintf(2, "Usage: find path filename [-exec cmd ...]\n");
    exit(1);
  }

  find(argv[1], argv[2], &argv[4], argc - 4);
  exit(0);
}
