#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int booted = 0;
volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(__sync_bool_compare_and_swap(&booted, 0, 1)){
    consoleinit();
    devnullinit();
    devzeroinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    futexinit();     // Linux futex wait lock
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    w_sie(r_sie() | SIE_SEIE | SIE_STIE);
    timerinit();
    intr_on();
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    vfsinit();       // virtual filesystem mount table
    virtio_disk_init(); // emulated hard disk
    virtio_net_init();
    netinit();
    userinit();      // first user process
    start_other_harts();
#ifdef KCSAN
    kcsaninit();
#endif
    __sync_synchronize();
    started = 1;
  } else {
    while(atomic_read4((int *) &started) == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    w_sie(r_sie() | SIE_SEIE | SIE_STIE);
    timerinit();
    intr_on();
    plicinithart();   // ask PLIC for device interrupts
  }
  scheduler();        
}
