#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S jumps here in supervisor mode on stack0.
void
start()
{
  // start with interrupts disabled until trap vectors are ready.
  intr_off();

  // disable paging for now
  w_satp(0);

  // already in supervisor mode; jump directly to main().
  main();
}

// ask each hart to generate timer interrupts.
void
timerinit()
{
  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
