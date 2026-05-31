// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *shargv[] = { "/bin/sh", 0 };

#ifndef NORMAL_BOOT
struct test_entry {
  char *dir;
  char *script;
};

struct test_entry tests[] = {
  { "/ext4/musl", "./basic_testcode.sh" },
  { "/ext4/musl", "./busybox_testcode.sh" },
  { "/ext4/musl", "./libctest_testcode.sh" },
  { "/ext4/glibc", "./basic_testcode.sh" },
  { "/ext4/glibc", "./busybox_testcode.sh" },
  //{ "/ext4/glibc", "./libctest_testcode.sh" },
  { 0, 0 },
};

static void
run_test(struct test_entry *test)
{
  char *argv[] = { test->script, 0 };
  int pid;
  int fd;

  if(chdir(test->dir) < 0){
    printf("init: cannot cd %s\n", test->dir);
    return;
  }

  fd = open(test->script, O_RDONLY);
  if(fd < 0){
    printf("init: skip missing %s/%s\n", test->dir, test->script);
    return;
  }
  close(fd);

  pid = fork();
  if(pid < 0){
    printf("init: fork failed for %s/%s\n", test->dir, test->script);
    return;
  }
  if(pid == 0){
    exec(test->script, argv);
    printf("init: exec %s/%s failed\n", test->dir, test->script);
    exit(1);
  }
  wait((int *)0);
}
#endif

int
main(void)
{
  int pid, wpid;

  if(open("/dev/console", O_RDWR) < 0){
    mkdir("/dev");
    mknod("/dev/console", CONSOLE, 0);
    mknod("/dev/null", DEVNULL, 0);
    mknod("/dev/zero", DEVZERO, 0);
    open("/dev/console", O_RDWR);
    printf("init: failed to open console\n");
  } 
  mknod("/dev/null", DEVNULL, 0);
  mknod("/dev/zero", DEVZERO, 0);
  mkdir("/ext4");
  mkdir("/tmp");
  mkdir("/dev/shm");
  dup(0);  // stdout
  dup(0);  // stderr

#ifndef NORMAL_BOOT
  for(struct test_entry *test = tests; test->script; test++)
    run_test(test);
  halt();
#endif

  for(;;){
    printf("init: starting sh\n");
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      exec("/bin/sh", shargv);
      printf("init: exec sh failed\n");
      exit(1);
    }

    for(;;){
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *) 0);
      if(wpid == pid){
        // the shell exited; restart it.
        break;
      } else if(wpid < 0){
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
