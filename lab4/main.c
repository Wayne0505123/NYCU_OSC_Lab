#include "mm.h"
#include <stdint.h>
#include <stddef.h>

/* ---------- UART from uart.c ---------- */
extern char uart_getc_polling(void);
extern void uart_putc_polling(char c);
extern void uart_puts_polling(const char *s);
extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern unsigned char uart_getb(void);
extern void uart_putb(unsigned char c);
extern void uart_set_base(unsigned long base);
extern void uart_set_config(int reg_shift, int reg_io_width);
extern unsigned int uart_read_reg(int off);
extern void uart_write_reg(int off, unsigned int val);
static volatile unsigned long uart_rx_irq_count = 0;
static volatile unsigned long ext_irq_count = 0;
static volatile unsigned long uart_irq_count = 0;
static volatile int last_irq = 0;

/* ---------- Linker symbols ---------- */
extern char _start[];
extern char _end[];
void start_kernel(const void *fdt);
extern void handle_exception(void);

/* ---------- Boot / load / relocation ---------- */
#define LOAD_ADDR   ((unsigned char *)0x00200000UL)
#define CPIO_LOAD_ADDR ((unsigned char *)0x03000000UL)
#define NEW_FDT_ADDR  ((unsigned char *)0x03F00000UL)
#define NEW_FDT_SIZE  0x400000UL
#define RELOC_ADDR  0x20000000UL
#define BOOT_MAGIC  0x544F4F42UL
#define KERNEL_STACK_SIZE (128 * 1024UL)

static const void *boot_fdt;

/* ---------- Orange Pi RV2 UART0 / PLIC ---------- */
#define UART_BASE 0xD4017000UL
#define UART_RBR 0
#define UART_THR 0
#define UART_IER 1
#define UART_IIR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5

#define UART_IER_RX   (1 << 0)
#define UART_IER_TX   (1 << 1)
#define UART_MCR_OUT2 (1 << 3)
#define UART_LSR_DR   (1 << 0)
#define UART_LSR_THRE (1 << 5)

#define PLIC_CONTEXT(hart)    ((hart) * 2 + 1)  /* S-mode context */

static unsigned long plic_base = 0;

static inline unsigned long plic_priority_addr(int irq) {
    return plic_base + (unsigned long)irq * 4UL;
}

static inline unsigned long plic_enable_addr(int ctx) {
    return plic_base + 0x2000UL + (unsigned long)ctx * 0x80UL;
}

static inline unsigned long plic_threshold_addr(int ctx) {
    return plic_base + 0x200000UL + (unsigned long)ctx * 0x1000UL;
}

static inline unsigned long plic_claim_addr(int ctx) {
    return plic_base + 0x200004UL + (unsigned long)ctx * 0x1000UL;
}

static unsigned long boot_cpu_hartid = 0;
static int uart_irq_id = 42;

static inline void write32(unsigned long addr, unsigned int value) {
    *(volatile unsigned int *)addr = value;
}

static inline unsigned int read32(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

static void dbg_delay(void) {
    for (volatile unsigned long i = 0; i < 200000; i++) {
        asm volatile("nop");
    }
}

static void dbg_putc(char c) {
    unsigned long timeout = 1000000;

    while (!(uart_read_reg(UART_LSR) & UART_LSR_THRE)) {
        if (--timeout == 0)
            return;
    }

    uart_write_reg(UART_THR, (unsigned char)c);
    dbg_delay();
}

static void dbg_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            dbg_putc('\r');
        dbg_putc(*s++);
    }
}

static unsigned int uart_ier_shadow;

static void uart_enable_tx_irq(void) {
    uart_ier_shadow |= UART_IER_RX | UART_IER_TX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void uart_disable_tx_irq(void) {
    uart_ier_shadow &= ~UART_IER_TX;
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}
/* ---------- CSR / trap constants ---------- */
#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP  (1UL << 8)

#define SIE_STIE     (1UL << 5)
#define SIE_SEIE     (1UL << 9)

#define SCAUSE_INTERRUPT (1UL << 63)
#define SCAUSE_U_ECALL   8UL
#define SCAUSE_S_TIMER   (SCAUSE_INTERRUPT | 5UL)
#define SCAUSE_S_EXT     (SCAUSE_INTERRUPT | 9UL)

struct pt_regs {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;
    unsigned long t0;
    unsigned long t1;
    unsigned long t2;
    unsigned long s0;
    unsigned long s1;
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;
    unsigned long a4;
    unsigned long a5;
    unsigned long a6;
    unsigned long a7;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
    unsigned long t3;
    unsigned long t4;
    unsigned long t5;
    unsigned long t6;
    unsigned long sepc;
    unsigned long sstatus;
    unsigned long scause;
    unsigned long stval;
};

/* ---------- SBI ---------- */
#define SBI_EXT_SET_TIMER 0x0
#define SBI_EXT_SHUTDOWN  0x8
#define SBI_EXT_BASE      0x10

struct sbiret {
    long error;
    long value;
};

static struct sbiret sbi_ecall(int ext, int fid,
                               unsigned long arg0,
                               unsigned long arg1,
                               unsigned long arg2,
                               unsigned long arg3,
                               unsigned long arg4,
                               unsigned long arg5) {
    struct sbiret ret;

    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = ext;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    ret.error = (long)a0;
    ret.value = (long)a1;
    return ret;
}

static void sbi_set_timer(unsigned long stime_value) {
    sbi_ecall(SBI_EXT_SET_TIMER, 0, stime_value, 0, 0, 0, 0, 0);
}

static unsigned long sbi_get_spec_version(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 0, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_id(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 1, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_version(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 2, 0, 0, 0, 0, 0, 0).value;
}

static inline unsigned long read_time(void) {
    unsigned long x;
    asm volatile("rdtime %0" : "=r"(x));
    return x;
}

static inline void local_irq_enable(void) {
    asm volatile("csrsi sstatus, 2" ::: "memory");
}

static inline void local_irq_disable(void) {
    asm volatile("csrci sstatus, 2" ::: "memory");
}

static void enable_timer_interrupt(void) {
    asm volatile("csrs sie, %0" :: "r"(SIE_STIE) : "memory");
}

static void enable_external_interrupt(void) {
    asm volatile("csrs sie, %0" :: "r"(SIE_SEIE) : "memory");
}

static inline void set_stvec_relocated(void) {
    unsigned long relocated_trap =
        RELOC_ADDR + ((unsigned long)handle_exception - (unsigned long)_start);
    asm volatile("csrw stvec, %0" :: "r"(relocated_trap) : "memory");
}

static inline unsigned long irq_save(void) {
    unsigned long flags;
    asm volatile("csrrci %0, sstatus, 2" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    if (flags & SSTATUS_SIE)
        local_irq_enable();
    else
        local_irq_disable();
}

/* ---------- tiny libc ---------- */
static size_t strlen_simple(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int strcmp_full(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strcmp_simple(const char *a, const char *b) {
    return strcmp_full(a, b) == 0;
}

static int strncmp_simple(const char *a, const char *b, size_t n) {
    while (n-- > 0) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
    }
    return 0;
}

static void strcpy_simple(char *dst, const char *src) {
    while ((*dst++ = *src++))
        ;
}

static void strncpy_message(char *dst, const char *src, size_t max) {
    size_t i = 0;
    if (max == 0)
        return;
    while (i + 1 < max && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void strcat_simple(char *dst, const char *src) {
    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
}

static int memcmp_simple(const void *s1, const void *s2, int n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    while (n-- > 0) {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
    }
    return 0;
}

static int hextoi_simple(const char *s, int n) {
    int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= '0' && *s <= '9')
            r += *s - '0';
        else if (*s >= 'A' && *s <= 'F')
            r += *s - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f')
            r += *s - 'a' + 10;
        s++;
    }
    return r;
}

static int align_int(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}

static const void *align_up_ptr(const void *ptr, size_t align) {
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static void uart_put_uint(unsigned long x) {
    char buf[32];
    int i = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }

    while (x > 0) {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0)
        uart_putc(buf[--i]);
}

/*
 * Trap/debug output must not depend on the asynchronous TX ring.
 * When a trap is taken from U-mode, SIE is cleared by hardware; if the
 * async console waits for TX interrupts to drain the ring, it can sleep in
 * wfi forever.  Keep trap diagnostics on polling UART.
 */
static void trap_puts(const char *s) {
    uart_puts_polling(s);
}

static void trap_put_uint(unsigned long x) {
    char buf[32];
    int i = 0;

    if (x == 0) {
        uart_putc_polling('0');
        return;
    }

    while (x > 0) {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0)
        uart_putc_polling(buf[--i]);
}

static void trap_hex(unsigned long h) {
    const char *hex = "0123456789abcdef";

    trap_puts("0x");
    for (int i = (int)(sizeof(unsigned long) * 2) - 1; i >= 0; i--)
        uart_putc_polling(hex[(h >> (i * 4)) & 0xf]);
}

/* ---------- FDT parser ---------- */
#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static uint32_t bswap32_main(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
}

static unsigned long read_cells(const uint32_t *p, int cells) {
    unsigned long v = 0;
    for (int i = 0; i < cells; i++)
        v = (v << 32) | bswap32_main(p[i]);
    return v;
}

static int fdt_is_valid(const void *fdt) {
    if (!fdt)
        return 0;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    return bswap32_main(hdr->magic) == 0xd00dfeed;
}

static int bytes_contains_string(const char *buf, int len, const char *needle) {
    size_t nlen = strlen_simple(needle);

    if (!buf || !needle || nlen == 0 || len <= 0)
        return 0;

    for (int i = 0; i + (int)nlen <= len; i++) {
        size_t j = 0;
        while (j < nlen && buf[i + (int)j] == needle[j])
            j++;
        if (j == nlen)
            return 1;
    }

    return 0;
}

int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (bswap32_main(hdr->magic) != 0xd00dfeed)
        return -1;

    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *p = struct_base;
    char curpath[1024];
    size_t pathlen_stack[128];
    int depth = 0;

    curpath[0] = '\0';

    while (1) {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE) {
            const char *name = p;
            size_t oldlen = strlen_simple(curpath);
            pathlen_stack[depth++] = oldlen;

            if (oldlen == 0) {
                strcpy_simple(curpath, "/");
            } else {
                if (strcmp_full(curpath, "/") != 0)
                    strcat_simple(curpath, "/");
                strcat_simple(curpath, name);
            }

            const char *a = curpath;
            const char *b = path;
            int matched = 1;

            while (*a || *b) {
                if (*a == '/' && *b == '/') {
                    a++;
                    b++;
                    continue;
                }

                while (*a && *b && *a != '/' && *b != '/' && *a == *b) {
                    a++;
                    b++;
                }

                if (!((*b == '\0' || *b == '/') &&
                      (*a == '\0' || *a == '/' || *a == '@'))) {
                    matched = 0;
                    break;
                }

                while (*a && *a != '/')
                    a++;
                while (*b && *b != '/')
                    b++;

                if ((*a == '\0') != (*b == '\0')) {
                    matched = 0;
                    break;
                }
            }

            if (matched)
                return nodeoff;

            p = (const char *)align_up_ptr(p + strlen_simple(name) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth > 0) {
                size_t oldlen = pathlen_stack[--depth];
                curpath[oldlen] = '\0';
            }
        } else if (tag == FDT_PROP) {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 8;
            p = (const char *)align_up_ptr(p + len, 4);
        } else if (tag == FDT_NOP) {
        } else if (tag == FDT_END) {
            break;
        } else {
            return -1;
        }
    }

    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset,
                        const char *name, int *lenp) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;

    if (bswap32_main(hdr->magic) != 0xd00dfeed)
        return 0;

    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *strings_base = (const char *)fdt + bswap32_main(hdr->off_dt_strings);
    const char *p = struct_base + nodeoffset;

    if (bswap32_main(*(const uint32_t *)p) != FDT_BEGIN_NODE)
        return 0;

    p += 4;
    p = (const char *)align_up_ptr(p + strlen_simple(p) + 1, 4);

    int depth = 0;

    while (1) {
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_PROP) {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 4;
            uint32_t nameoff = bswap32_main(*(const uint32_t *)p);
            p += 4;

            const char *prop_name = strings_base + nameoff;
            const void *prop_data = p;

            if (depth == 0 && strcmp_full(prop_name, name) == 0) {
                if (lenp)
                    *lenp = (int)len;
                return prop_data;
            }

            p = (const char *)align_up_ptr(p + len, 4);
        } else if (tag == FDT_BEGIN_NODE) {
            depth++;
            p = (const char *)align_up_ptr(p + strlen_simple(p) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth == 0)
                break;
            depth--;
        } else if (tag == FDT_NOP) {
        } else if (tag == FDT_END) {
            break;
        } else {
            return 0;
        }
    }

    return 0;
}


static void fdt_write_u64_prop(void *fdt, int node, const char *name,
                               unsigned long value) {
    int len = 0;
    uint32_t *prop = (uint32_t *)fdt_getprop(fdt, node, name, &len);

    if (!prop || len < 8) {
        uart_puts("fdt prop missing: ");
        uart_puts(name);
        uart_puts("\n");
        return;
    }

    prop[0] = bswap32_main((uint32_t)(value >> 32));
    prop[1] = bswap32_main((uint32_t)(value & 0xffffffffUL));
}

static void *make_writable_fdt_copy(const void *old_fdt) {
    if (!fdt_is_valid(old_fdt)) {
        uart_puts("invalid fdt\n");
        return 0;
    }

    const struct fdt_header *old_hdr = (const struct fdt_header *)old_fdt;
    unsigned int old_size = bswap32_main(old_hdr->totalsize);

    if (old_size > NEW_FDT_SIZE) {
        uart_puts("new fdt buffer too small\n");
        return 0;
    }

    unsigned char *dst = NEW_FDT_ADDR;
    const unsigned char *src = (const unsigned char *)old_fdt;

    for (unsigned int i = 0; i < old_size; i++)
        dst[i] = src[i];

    return (void *)dst;
}

static void update_initrd_in_fdt(void *fdt,
                                 unsigned long initrd_start_addr,
                                 unsigned long initrd_end_addr) {
    int chosen = fdt_path_offset(fdt, "/chosen");

    if (chosen < 0) {
        uart_puts("/chosen not found\n");
        return;
    }

    fdt_write_u64_prop(fdt, chosen, "linux,initrd-start", initrd_start_addr);
    fdt_write_u64_prop(fdt, chosen, "linux,initrd-end", initrd_end_addr);
}

static const void *initrd_start = 0;
static const void *initrd_end = 0;
static unsigned long timebase_frequency = 10000000UL;

static void initrd_init_from_dtb(const void *fdt) {
    int offset = fdt_path_offset(fdt, "/chosen");
    int len;

    if (offset < 0) {
        return;
    }

    const void *startp = fdt_getprop(fdt, offset, "linux,initrd-start", &len);
    if (startp)
        initrd_start = (const void *)read_cells((const uint32_t *)startp, len / 4);


    const void *endp = fdt_getprop(fdt, offset, "linux,initrd-end", &len);
    if (endp)
        initrd_end = (const void *)read_cells((const uint32_t *)endp, len / 4);


    uart_puts("initrd start = ");
    uart_hex((unsigned long)initrd_start);
    uart_puts("\n");
    uart_puts("initrd end   = ");
    uart_hex((unsigned long)initrd_end);
    uart_puts("\n");
}

static void uart_init_from_dtb(const void *fdt) {
    int len;
    unsigned long uart_base = UART_BASE;
    int reg_shift = 2;
    int reg_width = 4;

    int node = fdt_path_offset(fdt, "/soc/serial@d4017000");
    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/serial");
    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/uart");

    if (node < 0) {
        uart_set_base(uart_base);
        uart_set_config(reg_shift, reg_width);
        uart_irq_id = 42;
    } else {
        const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
        if (reg && len >= 16)
            uart_base = read_cells(reg, 2);


        int shift_len = 0;
        int width_len = 0;
        const uint32_t *shift = (const uint32_t *)fdt_getprop(fdt, node, "reg-shift", &shift_len);
        const uint32_t *width = (const uint32_t *)fdt_getprop(fdt, node, "reg-io-width", &width_len);
        reg_shift = (shift && shift_len >= 4) ? (int)bswap32_main(shift[0]) : 2;
        reg_width = (width && width_len >= 4) ? (int)bswap32_main(width[0]) : 4;

        const uint32_t *irq = (const uint32_t *)fdt_getprop(fdt, node, "interrupts", &len);
        if (irq && len >= 4)
            uart_irq_id = (int)bswap32_main(irq[0]);


        uart_set_base(uart_base);
        uart_set_config(reg_shift, reg_width);
    }

    uart_puts("uart base from dtb = ");
    uart_hex(uart_base);
    uart_puts("\n");
    uart_puts("uart reg shift = ");
    uart_hex((unsigned long)reg_shift);
    uart_puts("\n");
    uart_puts("uart reg width = ");
    uart_hex((unsigned long)reg_width);
    uart_puts("\n");
    uart_puts("uart irq = ");
    uart_put_uint((unsigned long)uart_irq_id);
    uart_puts("\n");
}

static int fdt_find_plic_node(const void *fdt) {
    if (!fdt_is_valid(fdt))
        return -1;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *p = struct_base;

    while (1) {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE) {
            const char *name = p;
            int len = 0;
            const char *compat = (const char *)fdt_getprop(fdt, nodeoff, "compatible", &len);
            const void *ndev = fdt_getprop(fdt, nodeoff, "riscv,ndev", 0);

            if ((compat &&
                 (bytes_contains_string(compat, len, "riscv,plic0") ||
                  bytes_contains_string(compat, len, "sifive,plic-1.0.0") ||
                  bytes_contains_string(compat, len, "plic"))) ||
                ndev) {
                return nodeoff;
            }

            p = (const char *)align_up_ptr(p + strlen_simple(name) + 1, 4);
        } else if (tag == FDT_PROP) {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 8;
            p = (const char *)align_up_ptr(p + len, 4);
        } else if (tag == FDT_END_NODE) {
        } else if (tag == FDT_NOP) {
        } else if (tag == FDT_END) {
            break;
        } else {
            return -1;
        }
    }

    return -1;
}

static void plic_init_from_dtb(const void *fdt) {
    int len = 0;
    int node = fdt_find_plic_node(fdt);

    if (node < 0) {
        return;
    }

    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
    if (!reg || len < 8) {
        return;
    }

    if (len >= 16)
        plic_base = read_cells(reg, 2);
    else
        plic_base = read_cells(reg, 1);

    uart_puts("plic base = ");
    uart_hex(plic_base);
    uart_puts("\n");
}

static void timer_frequency_init_from_dtb(const void *fdt) {
    int cpus = fdt_path_offset(fdt, "/cpus");
    if (cpus < 0) {
        return;
    }

    int len;
    const uint32_t *prop = (const uint32_t *)fdt_getprop(fdt, cpus, "timebase-frequency", &len);
    if (prop && len >= 4)
        timebase_frequency = bswap32_main(prop[0]);


    uart_puts("timebase-frequency = ");
    uart_put_uint(timebase_frequency);
    uart_puts("\n");
}

/* ---------- CPIO / initramfs ---------- */
struct cpio_t {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static void console_putc_async(char c);
static void console_puts_async(const char *s);
static void console_hex_async(unsigned long h);
static void console_put_uint_async(unsigned long x);

static void initrd_list(const void *rd) {
    const char *p = (const char *)rd;

    while (p && p < (const char *)initrd_end) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (memcmp_simple(hdr->magic, "070701", 6) != 0) {
            console_puts_async("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            return;

        console_put_uint_async((unsigned int)filesize);
		console_putc_async(' ');
		console_puts_async(name);
		console_putc_async('\n');

        const char *data = p + align_int(sizeof(struct cpio_t) + namesize, 4);
        p = data + align_int(filesize, 4);
    }
}

static const void *initrd_find(const char *filename, int *filesize_out) {
    const char *p = (const char *)initrd_start;

    while (p && p < (const char *)initrd_end) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (memcmp_simple(hdr->magic, "070701", 6) != 0)
            return 0;

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);
        const char *data = p + align_int(sizeof(struct cpio_t) + namesize, 4);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            break;

        const char *cmp_name = name;
        if (cmp_name[0] == '.' && cmp_name[1] == '/')
            cmp_name += 2;

        if (strcmp_full(cmp_name, filename) == 0) {
            if (filesize_out)
                *filesize_out = filesize;
            return data;
        }

        p = data + align_int(filesize, 4);
    }

    return 0;
}

static void initrd_cat(const void *rd, const char *filename) {
    (void)rd;
    int size = 0;
    const char *data = (const char *)initrd_find(filename, &size);

    if (!data) {
        console_puts_async("initrd_cat: ");
		console_puts_async(filename);
		console_puts_async(": No such file\n");
        return;
    }

    for (int i = 0; i < size; i++)
        console_putc_async(data[i]);
    console_putc_async('\n');
}

/* ---------- Ring buffer for asynchronous UART RX/TX ---------- */
#define RING_SIZE 256
struct ringbuf {
    char buf[RING_SIZE];
    volatile unsigned int r;
    volatile unsigned int w;
};

static struct ringbuf rx_ring;
static struct ringbuf tx_ring;
static int async_console_enabled = 0;

static int ring_empty(struct ringbuf *rb) {
    return rb->r == rb->w;
}

static int ring_full(struct ringbuf *rb) {
    return ((rb->w + 1) % RING_SIZE) == rb->r;
}

static void ring_push(struct ringbuf *rb, char c) {
    unsigned int next = (rb->w + 1) % RING_SIZE;
    if (next == rb->r)
        return;
    rb->buf[rb->w] = c;
    rb->w = next;
}

static int ring_pop(struct ringbuf *rb, char *c) {
    if (ring_empty(rb))
        return 0;
    *c = rb->buf[rb->r];
    rb->r = (rb->r + 1) % RING_SIZE;
    return 1;
}

static void ring_clear(struct ringbuf *rb) {
    rb->r = 0;
    rb->w = 0;
}

static void uart_kick_tx(void) {
    if (!ring_empty(&tx_ring) && (uart_read_reg(UART_LSR) & UART_LSR_THRE)) {
        char c;
        if (ring_pop(&tx_ring, &c))
            uart_write_reg(UART_THR, (unsigned char)c);
    }

    if (!ring_empty(&tx_ring))
        uart_enable_tx_irq();
    else
        uart_disable_tx_irq();
}

static void console_putc_async(char c) {
    unsigned long flags;

    if (!async_console_enabled) {
        uart_putc_polling(c);
        return;
    }

    if (c == '\n')
        console_putc_async('\r');

    while (1) {
        flags = irq_save();

        if (!ring_full(&tx_ring)) {
            ring_push(&tx_ring, c);
            uart_kick_tx();
            irq_restore(flags);
            return;
        }

        irq_restore(flags);
        asm volatile("wfi");
    }
}

static void console_puts_async(const char *s) {
    while (*s)
        console_putc_async(*s++);
}

static void console_hex_async(unsigned long h) {
    const char *hex = "0123456789abcdef";
    console_puts_async("0x");
    for (int i = (int)(sizeof(unsigned long) * 2) - 1; i >= 0; i--) {
        console_putc_async(hex[(h >> (i * 4)) & 0xf]);
    }
}

static void console_put_uint_async(unsigned long x) {
    char buf[32];
    int i = 0;

    if (x == 0) {
        console_putc_async('0');
        return;
    }

    while (x > 0) {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0)
        console_putc_async(buf[--i]);
}

/* ---------- Task queue: advanced exercise 2 ---------- */
typedef void (*task_callback_t)(void *arg);

#define MAX_TASKS 64
struct task {
    int used;
    int priority;
    unsigned long seq;
    task_callback_t cb;
    void *arg;
};

static struct task tasks[MAX_TASKS];
static unsigned long task_seq = 0;
static int current_task_priority = -1;
static int tasks_running = 0;

void add_task(task_callback_t callback, void *arg, int priority) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used) {
            tasks[i].used = 1;
            tasks[i].priority = priority;
            tasks[i].seq = task_seq++;
            tasks[i].cb = callback;
            tasks[i].arg = arg;
            return;
        }
    }

    uart_puts("[Task] queue full\n");
}

static int pick_task_above(int min_priority) {
    int best = -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used)
            continue;
        if (tasks[i].priority <= min_priority)
            continue;
        if (best < 0 ||
            tasks[i].priority > tasks[best].priority ||
            (tasks[i].priority == tasks[best].priority && tasks[i].seq < tasks[best].seq)) {
            best = i;
        }
    }

    return best;
}

static void run_tasks(void) {
    unsigned long flags = irq_save();

    int previous_running = tasks_running;
    int previous_priority = current_task_priority;

    tasks_running = 1;

    while (1) {
        int idx = pick_task_above(current_task_priority);
        if (idx < 0)
            break;

        task_callback_t cb = tasks[idx].cb;
        void *arg = tasks[idx].arg;
        int prio = tasks[idx].priority;

        tasks[idx].used = 0;

        int saved_priority = current_task_priority;
        current_task_priority = prio;

        local_irq_enable();
        cb(arg);
        local_irq_disable();

        current_task_priority = saved_priority;
    }

    tasks_running = previous_running;
    current_task_priority = previous_priority;

    irq_restore(flags);
}

struct uart_rx_arg {
    int used;
    char c;
};

static struct uart_rx_arg uart_rx_args[128];

static void uart_rx_task(void *arg) {
    struct uart_rx_arg *rx = (struct uart_rx_arg *)arg;
    ring_push(&rx_ring, rx->c == '\r' ? '\n' : rx->c);
    rx->used = 0;
}

static void enqueue_uart_rx_task(char c) {
    for (int i = 0; i < 128; i++) {
        if (!uart_rx_args[i].used) {
            uart_rx_args[i].used = 1;
            uart_rx_args[i].c = c;
            add_task(uart_rx_task, &uart_rx_args[i], 3);
            return;
        }
    }
}

static char console_getc(void) {
    char c;

    while (!ring_pop(&rx_ring, &c)) {
        run_tasks();
        asm volatile("wfi");
    }

    return c == '\r' ? '\n' : c;
}

/*
 * Public UART console API.
 *
 * Lab4 asks uart_getc/uart_putc/uart_puts to be asynchronous.  The old
 * busy-wait implementations are kept in uart.c as *_polling and are used
 * only before interrupts/ring buffers are enabled or during binary load.
 */
char uart_getc(void) {
    if (!async_console_enabled)
        return uart_getc_polling();

    return console_getc();
}

void uart_putc(char c) {
    console_putc_async(c);
}

void uart_puts(const char *s) {
    console_puts_async(s);
}

static void debug_putc_nowait(char c) {
    if (uart_read_reg(UART_LSR) & UART_LSR_THRE)
        uart_write_reg(UART_THR, (unsigned char)c);
}
/* ---------- PLIC / UART interrupt ---------- */


static void plic_init(void) {
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base) {
        return;
    }

    write32(plic_priority_addr(uart_irq_id), 1);

    unsigned long enable_addr =
        plic_enable_addr(ctx) + (unsigned long)(uart_irq_id / 32) * 4UL;

    unsigned int enable = read32(enable_addr);
    enable |= (1U << (uart_irq_id % 32));
    write32(enable_addr, enable);

    write32(plic_threshold_addr(ctx), 0);
}

static int plic_claim(void) {
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base)
        return 0;

    return (int)read32(plic_claim_addr(ctx));
}

static void plic_complete(int irq) {
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base)
        return;

    write32(plic_claim_addr(ctx), (unsigned int)irq);
}

#define UART_LCR_8N1 0x03

static void uart_interrupt_init(void) {
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);

    unsigned int mcr = uart_read_reg(UART_MCR);
    uart_write_reg(UART_MCR, mcr | UART_MCR_OUT2);
}

static void uart_interrupt_disable(void) {
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow &= ~(UART_IER_RX | UART_IER_TX);
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void uart_interrupt_enable(void) {
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void handle_uart_interrupt(void) {
    while (uart_read_reg(UART_LSR) & UART_LSR_DR) {
        char c = (char)(uart_read_reg(UART_RBR) & 0xff);
        ring_push(&rx_ring, c == '\r' ? '\n' : c);
    }

    while (!ring_empty(&tx_ring) &&
           (uart_read_reg(UART_LSR) & UART_LSR_THRE)) {
        char c;
        if (ring_pop(&tx_ring, &c))
            uart_write_reg(UART_THR, (unsigned char)c);
    }

    if (ring_empty(&tx_ring))
        uart_disable_tx_irq();
    else
        uart_enable_tx_irq();
}

/* ---------- Timer multiplexing: advanced exercise 1 ---------- */
typedef void (*timer_callback_t)(void *arg);

static int timer_boot_log_enabled = 0;

static void boot_time_task(void *arg) {
    unsigned long sec = (unsigned long)arg;

    console_puts_async("boot time: ");
    console_put_uint_async(sec);
    console_puts_async("\n");
}

#define MAX_TIMERS 64
struct timer_event {
    int used;
    unsigned long expire;
    timer_callback_t cb;
    void *arg;
};

static struct timer_event timers[MAX_TIMERS];
static unsigned long next_periodic_tick = 0;
static unsigned long boot_time_base = 0;

static unsigned long ticks_per_sec(void) {
    return timebase_frequency ? timebase_frequency : 10000000UL;
}

static unsigned long now_seconds(void) {
    return (read_time() - boot_time_base) / ticks_per_sec();
}

static void program_next_timer(void) {
    unsigned long next = next_periodic_tick;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used)
            continue;
        if (next == 0 || timers[i].expire < next)
            next = timers[i].expire;
    }

    if (next == 0)
        next = read_time() + 2 * ticks_per_sec();

    sbi_set_timer(next);
}

void add_timer(timer_callback_t callback, void *arg, int sec) {
    if (sec < 0)
        sec = 0;

    unsigned long expire = read_time() + (unsigned long)sec * ticks_per_sec();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used) {
            timers[i].used = 1;
            timers[i].expire = expire;
            timers[i].cb = callback;
            timers[i].arg = arg;
            program_next_timer();
            return;
        }
    }

    console_puts_async("[Timer] queue full\n");
}

struct timeout_message {
    int used;
    unsigned long created_sec;
    int seconds;
    char msg[96];
};

static struct timeout_message timeout_messages[MAX_TIMERS];

static void timeout_task(void *arg) {
    struct timeout_message *tm = (struct timeout_message *)arg;

    console_puts_async(tm->msg);
    console_puts_async("\n");

    tm->used = 0;
}

static void timeout_timer_cb(void *arg) {
    add_task(timeout_task, arg, 2);
}

static void handle_timer_interrupt(void) {
    unsigned long now = read_time();

    while (next_periodic_tick && now >= next_periodic_tick) {
        if (timer_boot_log_enabled) {
			unsigned long sec =
				(next_periodic_tick - boot_time_base) / ticks_per_sec();

			add_task(boot_time_task, (void *)sec, 2);
		}

        next_periodic_tick += 2 * ticks_per_sec();
    }

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used && now >= timers[i].expire) {
            timer_callback_t cb = timers[i].cb;
            void *arg = timers[i].arg;
            timers[i].used = 0;
            add_task((task_callback_t)cb, arg, 2);
        }
    }

    program_next_timer();
}

static void timer_init(void) {
    boot_time_base = read_time();
    next_periodic_tick = boot_time_base;

    /*
     * Do not enable STIE here.
     * start_kernel() will enable it after printing the prompt.
     */
    program_next_timer();
}

/* ---------- exec user program: basic exercise 1 ---------- */
static unsigned long user_return_addr = 0;
static unsigned long user_kernel_sp = 0;
static int user_mode_running = 0;

#define SYS_EXIT 93

static int exec_user_program(const char *filename) {
    int filesize = 0;
    const char *data = (const char *)initrd_find(filename, &filesize);

    if (!data) {
        uart_puts("Failed to exec user program!\n");
        return -1;
    }

    void *stack_page = allocate(PAGE_SIZE);
    if (!stack_page) {
        uart_puts("Failed to allocate user stack\n");
        return -1;
    }

    unsigned long user_entry = (unsigned long)data;
    unsigned long user_stack = (unsigned long)stack_page + PAGE_SIZE;

    unsigned long kernel_sp;
    asm volatile("mv %0, sp" : "=r"(kernel_sp));

    unsigned long sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus &= ~SSTATUS_SPP;
    sstatus |= SSTATUS_SPIE;
    
    user_mode_running = 1;
    user_return_addr = (unsigned long)&&user_return;
    user_kernel_sp = kernel_sp;

    asm volatile(
        "csrw sepc, %0\n"
        "csrw sstatus, %1\n"
        "csrw sscratch, %2\n"
        "mv sp, %3\n"
        "sret\n"
        :
        : "r"(user_entry),
          "r"(sstatus),
          "r"(kernel_sp),
          "r"(user_stack)
        : "memory");

user_return:
    user_mode_running = 0;
    user_return_addr = 0;
    user_kernel_sp = 0;

    uart_puts("[User] program exited\n");
    free(stack_page);
    (void)filesize;
    return 0;
}

/* ---------- trap handler ---------- */
void do_trap(struct pt_regs *regs) {
    unsigned long scause = regs->scause;

    if (scause == SCAUSE_U_ECALL) {
        trap_puts("=== S-Mode trap ===\n");
        trap_puts("scause: ");
        trap_put_uint(regs->scause);
        trap_puts("\n");
        trap_puts("sepc: ");
        trap_hex(regs->sepc);
        trap_puts("\n");
        trap_puts("stval: ");
        trap_put_uint(regs->stval);
        trap_puts("\n");

        if (user_mode_running && user_return_addr) {
    	    if (regs->a7 == SYS_EXIT) {
        	regs->sepc = user_return_addr;
        	regs->sstatus |= SSTATUS_SPP;
        	regs->sstatus |= SSTATUS_SPIE;
        	regs->sp = user_kernel_sp;
    	    } else {
        
        	regs->sepc += 4;
    	    }
	} else {
    		regs->sepc += 4;
	}
    } else if (scause == SCAUSE_S_TIMER) {
		handle_timer_interrupt();
	} else if (scause == SCAUSE_S_EXT) {
		ext_irq_count++;

		int irq = plic_claim();
		last_irq = irq;

		if (irq == uart_irq_id) {
		    uart_irq_count++;
		    handle_uart_interrupt();
		}

		if (irq)
		    plic_complete(irq);
	} else {
        trap_puts("Unexpected trap. sepc: ");
        trap_hex(regs->sepc);
        trap_puts(", scause: ");
        trap_hex(regs->scause);
        trap_puts(", stval: ");
        trap_hex(regs->stval);
        trap_puts("\n");
        while (1) {}
    }

    run_tasks();
}

/* ---------- load command ---------- */
static unsigned int uart_get_u32_polling(void) {
    unsigned int x = 0;
    x |= (unsigned int)uart_getb();
    x |= (unsigned int)uart_getb() << 8;
    x |= (unsigned int)uart_getb() << 16;
    x |= (unsigned int)uart_getb() << 24;
    return x;
}

static void boot_loaded_kernel(const void *fdt) {
    void (*kernel_entry)(unsigned long hartid, const void *fdt);
    kernel_entry = (void (*)(unsigned long, const void *))LOAD_ADDR;
    asm volatile("fence.i" ::: "memory");
    kernel_entry(0, fdt);
}

static void shell_load(void) {
    int old_timer_log = timer_boot_log_enabled;

    timer_boot_log_enabled = 0;

    uart_interrupt_disable();

    ring_clear(&rx_ring);
    ring_clear(&tx_ring);

    async_console_enabled = 0;

    uart_puts("Waiting for kernel image...\n");

    unsigned int magic = uart_get_u32_polling();

    if (magic != BOOT_MAGIC) {
        uart_puts("Bad magic.\n");

        async_console_enabled = 1;
        uart_interrupt_enable();
        timer_boot_log_enabled = old_timer_log;
        return;
    }

    unsigned int size = uart_get_u32_polling();

    uart_puts("Receiving kernel...\n");

    unsigned char *dst = (unsigned char *)LOAD_ADDR;
    for (unsigned int i = 0; i < size; i++) {
		dst[i] = uart_getb();
	}

    uart_puts("Waiting for cpio archive...\n");

    magic = uart_get_u32_polling();
    if (magic != BOOT_MAGIC) {
        uart_puts("Bad cpio magic.\n");
        async_console_enabled = 1;
        uart_interrupt_enable();
        timer_boot_log_enabled = old_timer_log;
        return;
    }

    unsigned int cpio_size = uart_get_u32_polling();
    uart_puts("Receiving cpio...\n");

    unsigned char *cpio_dst = (unsigned char *)CPIO_LOAD_ADDR;
    for (unsigned int i = 0; i < cpio_size; i++)
        cpio_dst[i] = uart_getb();

    void *new_fdt = make_writable_fdt_copy(boot_fdt);
    if (!new_fdt) {
        async_console_enabled = 1;
        uart_interrupt_enable();
        timer_boot_log_enabled = old_timer_log;
        return;
    }

    update_initrd_in_fdt(new_fdt,
                         (unsigned long)CPIO_LOAD_ADDR,
                         (unsigned long)CPIO_LOAD_ADDR + cpio_size);

    uart_puts("new fdt = ");
    uart_hex((unsigned long)new_fdt);
    uart_puts("\n");
    uart_puts("new initrd start = ");
    uart_hex((unsigned long)CPIO_LOAD_ADDR);
    uart_puts("\n");
    uart_puts("new initrd end   = ");
    uart_hex((unsigned long)CPIO_LOAD_ADDR + cpio_size);
    uart_puts("\n");

    uart_puts("Booting loaded kernel...\n");

    local_irq_disable();
    asm volatile("csrc sie, %0" :: "r"(SIE_STIE | SIE_SEIE) : "memory");

    boot_loaded_kernel(new_fdt);
}


/* ---------- shell ---------- */
static void print_prompt(void) {
    console_puts_async("opi-rv2> ");
}

static int parse_uint(const char **p) {
    int v = 0;
    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

static void test_task_cb(void *arg) {
    char *s = (char *)arg;
    console_puts_async("[Task] Executing Priority ");
    console_puts_async(s);
    console_puts_async("\n");
}

static void shell_help(void) {
    console_puts_async("Available commands:\n");
    console_puts_async("    help        - show all commands.\n");
    console_puts_async("    hello       - print Hello world.\n");
    console_puts_async("    info        - print system info.\n");
    console_puts_async("    load        - load a kernel and cpio over UART.\n");
    console_puts_async("    ls          - list files.\n");
    console_puts_async("    cat         - show file content.\n");
    console_puts_async("    memtest     - run memory allocator test.\n");
    console_puts_async("    exec        - execute a user program.\n");
    console_puts_async("    settimeout  - show text after X sec.\n");
    console_puts_async("    tasktest    - test priority task queue.\n");
}

static void shell_info(void) {
    console_puts_async("System information:\n");
    console_puts_async("    OpenSBI specification version: ");
    console_hex_async(sbi_get_spec_version());
    console_puts_async("\n");

    console_puts_async("    implementation ID: ");
    console_hex_async(sbi_get_impl_id());
    console_puts_async("\n");

    console_puts_async("    implementation version: ");
    console_hex_async(sbi_get_impl_version());
    console_puts_async("\n");

    console_puts_async("    timebase-frequency: ");
    console_put_uint_async(timebase_frequency);
    console_puts_async("\n");
}

static void shell_set_timeout(const char *cmd) {
    const char *p = cmd;
    while (*p && *p != ' ')
        p++;

    int sec = parse_uint(&p);

    while (*p == ' ')
        p++;

    if (*p == '\0') {
        console_puts_async("Usage: settimeout SECONDS MESSAGE\n");
        return;
    }

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timeout_messages[i].used) {
            timeout_messages[i].used = 1;
            timeout_messages[i].created_sec = now_seconds();
            timeout_messages[i].seconds = sec;
            strncpy_message(timeout_messages[i].msg, p, sizeof(timeout_messages[i].msg));
            add_timer(timeout_timer_cb, &timeout_messages[i], sec);
            return;
        }
    }

    console_puts_async("settimeout queue full\n");
}

static void shell_execute(const char *cmd) {
    if (strcmp_simple(cmd, "help")) {
        shell_help();
    } else if (strcmp_simple(cmd, "hello")) {
        console_puts_async("Hello world.\n");
    } else if (strcmp_simple(cmd, "info")) {
        shell_info();
    } else if (strcmp_simple(cmd, "load")) {
        shell_load();
    } else if (strcmp_simple(cmd, "ls")) {
        if (initrd_start)
            initrd_list(initrd_start);
        else
            console_puts_async("initrd not found\n");
    } else if (strncmp_simple(cmd, "cat ", 4) == 0) {
        if (initrd_start)
            initrd_cat(initrd_start, cmd + 4);
        else
            console_puts_async("initrd not found\n");
    } else if (strcmp_simple(cmd, "memtest")) {
        test_alloc_1();
    } else if (strcmp_simple(cmd, "exec")) {
        exec_user_program("prog.bin");
    } else if (strncmp_simple(cmd, "exec ", 5) == 0) {
        exec_user_program(cmd + 5);
    } else if (strncmp_simple(cmd, "setTimeout ", 11) == 0 ||
               strncmp_simple(cmd, "settimeout ", 11) == 0) {
        shell_set_timeout(cmd);
    } else if (strcmp_simple(cmd, "tasktest")) {
        add_task(test_task_cb, "1", 1);
        add_task(test_task_cb, "3", 3);
        add_task(test_task_cb, "2", 2);
        run_tasks();
    } else if (cmd[0] != '\0') {
        console_puts_async("Unknown command: ");
		console_puts_async(cmd);
		console_puts_async("\nUse help to get commands.\n");
    }
}

/* ---------- self relocation ---------- */
static void relocate_self(const void *fdt) {
    unsigned char *src = (unsigned char *)_start;
    unsigned char *dst = (unsigned char *)RELOC_ADDR;
    unsigned long size = (unsigned long)(_end - _start);

    for (unsigned long i = 0; i < size; i++)
        dst[i] = src[i];

    unsigned long kernel_size =
    (unsigned long)_end - (unsigned long)_start;

	unsigned long new_sp =
		RELOC_ADDR + kernel_size + KERNEL_STACK_SIZE;

	new_sp &= ~0xFUL;

	asm volatile("mv sp, %0" :: "r"(new_sp) : "memory");

    void (*entry)(const void *) =
        (void (*)(const void *))(RELOC_ADDR + ((unsigned long)start_kernel - (unsigned long)_start));

    asm volatile("fence.i" ::: "memory");
    entry(fdt);
}

/* ---------- kernel entry ---------- */
static void debug_putc_timeout(char c) {
    unsigned long timeout = 100000;

    while (!(uart_read_reg(UART_LSR) & UART_LSR_THRE)) {
        if (--timeout == 0)
            return;
    }

    uart_write_reg(UART_THR, (unsigned char)c);
}

static void delay_loop(unsigned long n) {
    while (n--) {
        asm volatile("nop");
    }
}

void start_kernel(const void *fdt) {
    /*
     * Important for hot-loaded kernel:
     * Previous kernel may jump here with SIE/STIE/SEIE still enabled.
     */
    local_irq_disable();
    asm volatile("csrc sie, %0" :: "r"(SIE_STIE | SIE_SEIE) : "memory");

    boot_fdt = fdt;
    uart_set_base(UART_BASE);

    if ((unsigned long)_start < RELOC_ADDR) {
        uart_puts("\nRelocating kernel...\n");
        relocate_self(fdt);
        while (1) {}
    }

    set_stvec_relocated();

    char buf[160];
    int idx = 0;

    uart_puts("\nStarting kernel ...\n");

    uart_puts("fdt ptr = ");
    uart_hex((unsigned long)fdt);
    uart_puts("\n");

    uart_init_from_dtb(fdt);
    initrd_init_from_dtb(fdt);
    mm_init_advanced(fdt, (unsigned long)initrd_start, (unsigned long)initrd_end);
    uart_puts("memory allocator ready\n");
    timer_frequency_init_from_dtb(fdt);
    plic_init_from_dtb(fdt);

    plic_init();
    uart_interrupt_init();
    timer_init();

	enable_timer_interrupt();
	enable_external_interrupt();
	local_irq_enable();

	async_console_enabled = 1;
	
	console_puts_async("\nType help to get commands.\n\n");
	console_puts_async("opi-rv2> ");
	
	add_task(boot_time_task, (void *)0, 2);
	timer_boot_log_enabled = 1;	

	while (1) {
		char c = console_getc();

		if (c == '\r' || c == '\n') {
		    console_putc_async('\n');
		    buf[idx] = '\0';

		    shell_execute(buf);

		    idx = 0;
		    console_puts_async("\nopi-rv2> ");
		} else if (c == 127 || c == '\b') {
		    if (idx > 0) {
		        idx--;
		        console_puts_async("\b \b");
		    }
		} else {
		    if (idx < (int)sizeof(buf) - 1) {
		        buf[idx++] = c;
		        console_putc_async(c);
		    }
		}
	}
}
