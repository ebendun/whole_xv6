#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"

extern void _entry(void);

#define SBI_EXT_HSM 0x48534d
#define SBI_HSM_HART_START 0
#define SBI_HSM_HART_GET_STATUS 2
#define SBI_HSM_HART_STARTED 0

struct sbiret {
  long error;
  long value;
};

static struct sbiret
sbi_call(long ext, long fid, long arg0, long arg1, long arg2)
{
  register long a0 asm("a0") = arg0;
  register long a1 asm("a1") = arg1;
  register long a2 asm("a2") = arg2;
  register long a6 asm("a6") = fid;
  register long a7 asm("a7") = ext;
  asm volatile("ecall"
               : "+r"(a0), "+r"(a1)
               : "r"(a2), "r"(a6), "r"(a7)
               : "memory");
  return (struct sbiret){a0, a1};
}

//cause using the opensbi we have to start the other hart by ourselves
// start the other harts, which will enter kernel in _entry.S, and become scheduler threads.
void
start_other_harts()
{
  int me = cpuid();
  struct sbiret ret;

  for(int hart = 0; hart < NCPU; hart++){
    if(hart == me)
      continue;
    ret = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START, hart, (uint64)_entry, 0);
    if(ret.error < 0)
      continue;

    for(int i = 0; i < 100000; i++){
      ret = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_GET_STATUS, hart, 0, 0);
      if(ret.error < 0 || ret.value == SBI_HSM_HART_STARTED)
        break;
    }
  }
}
