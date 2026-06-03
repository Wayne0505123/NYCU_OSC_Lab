OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0xffffffc000000000;
    . += 0x00200000;
    _start = .;
    .text : { *(.text.boot) *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .page_table (NOLOAD) : {
        . = ALIGN(4096);
        *(.page_table)
    }
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    }
    . = ALIGN(16);
    . = . + 0x4000;
    _end = .;
}
