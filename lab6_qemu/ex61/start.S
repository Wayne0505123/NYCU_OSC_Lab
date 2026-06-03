.section ".text.boot"
.global _start
_start:
    la      t0, 1f
    li      t1, 0xffffffc000000000
    add     t0, t0, t1
    la      sp, _end
    call    setup_vm
    jr      t0

.align 2
1:

    la      sp, _end

    la      a3, __bss_start
    la      a4, __bss_stop
    ble     a4, a3, clear_bss_done
clear_bss:
    sd      zero, (a3)
    add     a3, a3, 8
    blt     a3, a4, clear_bss
clear_bss_done:
    call    drop_identity_map
    tail    start_kernel
