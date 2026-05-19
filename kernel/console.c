//
// Console input and output, to the uart.
// Reads are character-oriented; user/sh.c owns line editing.
// Implements special input characters:
//   control-d -- end of file
//   control-p -- print process list
//

#include "stdarg.h"

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32];
  int i = 0;

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// append to cons.buf and wake readers. The shell consumes escape sequences,
// redraws the line, and implements history/completion.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  default:
    if(c > 0 && c < 0x80 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;
      cons.w = cons.e;
      wakeup(&cons.r);
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}

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
