.section ".text.boot"
.global _start
_start:
    la t0, __bss_start
    la t1, __bss_stop
1:
    bgeu t0, t1, 2f
    sb zero, 0(t0)
    addi t0, t0, 1
    j 1b
2:
    la sp, _end
    tail start_kernel
