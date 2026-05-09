#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();
void start_other_harts();

extern void _entry(void);

#define SBI_EXT_HSM 0x48534d
#define SBI_HSM_HART_START 0

static inline long
sbi_call(long ext, long fid, long arg0, long arg1, long arg2)
{
  register long a0 asm("a0") = arg0;
  register long a1 asm("a1") = arg1;
  register long a2 asm("a2") = arg2;
  register long a6 asm("a6") = fid;
  register long a7 asm("a7") = ext;
  asm volatile("ecall"
               : "+r"(a0)
               : "r"(a1), "r"(a2), "r"(a6), "r"(a7)
               : "memory");
  return a0;
}

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S jumps here in supervisor mode on stack0.
void
start()
{
  // start with interrupts disabled until trap vectors are ready.
  intr_off();

  // disable paging for now.
  w_satp(0);

  // already in supervisor mode; jump directly to main().
  main();
}

void
start_other_harts()
{
  int me = cpuid();

  for(int hart = 0; hart < NCPU; hart++){
    if(hart == me)
      continue;
    sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START, hart, (uint64)_entry, 0);
  }
}

// ask each hart to generate timer interrupts.
void
timerinit()
{
  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
