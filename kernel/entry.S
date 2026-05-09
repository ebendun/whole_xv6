        # OpenSBI loads the kernel at 0x80200000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80200000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # OpenSBI provides hartid in a0.
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a1, 1024*4
        addi a2, a0, 1
        mul a1, a1, a2
        add sp, sp, a1
        # keep hartid in tp for cpuid()
        mv tp, a0
        # jump to start() in start.c
        call start
spin:
        j spin
