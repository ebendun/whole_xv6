// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 0
// 10002000 -- virtio disk 1
// 80200000 -- OpenSBI loads the kernel here,
//             then jumps here.
// unused RAM after 80200000.

// the kernel uses physical memory thus:
// 80200000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// first virtio mmio interface (for ext4 fs)
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1
#define VIRTIO_COUNT 8

// second virtio mmio interface (for xv6 fs)
#define VIRTIO1 (VIRTIO0 + 0x1000)
#define VIRTIO1_IRQ 2

// third virtio mmio interface (for virtio-net)
#define VIRTIO2 (VIRTIO0 + 0x2000)
#define VIRTIO2_IRQ 3

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80200000 to PHYSTOP.
#define KERNBASE 0x80200000L
#define PHYSTOP (KERNBASE + 256*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK_PAGES 2
#define KSTACK(p) (TRAMPOLINE - ((p)*(KSTACK_PAGES+1) + KSTACK_PAGES + 2)*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   per-thread trapframe slots
//   USIGRETURN (shared user executable Linux rt_sigreturn stub)
//   TRAMPOLINE (the same page as in the kernel)
#define USIGRETURN (TRAMPOLINE - PGSIZE)
#define TRAPFRAME_BASE (USIGRETURN - PGSIZE)
#define TRAPFRAME_SLOT(i) (TRAPFRAME_BASE - ((i) * PGSIZE))
#define TRAPFRAME TRAPFRAME_SLOT(0)
#define TRAPFRAME_SLOTS NPROC
#define MMAP_TOP TRAPFRAME_SLOT(TRAPFRAME_SLOTS)

#ifndef __ASSEMBLER__
struct usyscall {
  int pid;  // Process ID
};
#endif
