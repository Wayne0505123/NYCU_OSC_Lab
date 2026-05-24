.section ".text.boot"
.global _start
_start:
    /* Clear .bss */
    la a3, __bss_start
    la a4, __bss_stop
    ble a4, a3, clear_bss_done
clear_bss:
    sd zero, (a3)
    addi a3, a3, 8
    blt a3, a4, clear_bss
clear_bss_done:
    /* OpenSBI enters with hartid in a0 and FDT pointer in a1. */
    la sp, _end
    mv a0, a1
    tail start_kernel
