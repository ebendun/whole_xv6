#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "riscv.h"
#include "defs.h"

static int
devnullread(int user_dst, uint64 dst, int n)
{
  (void)user_dst;
  (void)dst;
  (void)n;
  return 0;
}

static int
devnullwrite(int user_src, uint64 src, int n)
{
  (void)user_src;
  (void)src;
  return n;
}

void
devnullinit(void)
{
  devsw[DEVNULL].read = devnullread;
  devsw[DEVNULL].write = devnullwrite;
}

static int
devzeroread(int user_dst, uint64 dst, int n)
{
  char zeros[32];
  int i = 0;

  memset(zeros, 0, sizeof(zeros));
  while(i < n){
    int nn = sizeof(zeros);
    if(nn > n - i)
      nn = n - i;
    if(either_copyout(user_dst, dst + i, zeros, nn) < 0)
      break;
    i += nn;
  }
  return i;
}

void
devzeroinit(void)
{
  devsw[DEVZERO].read = devzeroread;
  devsw[DEVZERO].write = devnullwrite;
}
