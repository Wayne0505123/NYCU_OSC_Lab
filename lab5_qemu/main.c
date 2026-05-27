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
struct pt_regs;
extern void video_init(void);
extern void video_bmp_display(unsigned int *bmp_image, int width, int height);
extern void switch_to(void *prev, void *next);
extern void restore_trap_frame(struct pt_regs *regs);

/* ---------- Linker symbols ---------- */
extern char _start[];
extern char _end[];
void start_kernel(const void *fdt);
extern void handle_exception(void);

/* ---------- Boot / load / relocation ---------- */
#define LOAD_ADDR ((unsigned char *)0x84000000UL)
#define CPIO_LOAD_ADDR ((unsigned char *)0x88000000UL)
#define NEW_FDT_ADDR ((unsigned char *)0x83F00000UL)
#define NEW_FDT_SIZE 0x400000UL
#define RELOC_ADDR 0x80200000UL
#define BOOT_MAGIC 0x544F4F42UL
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

#define UART_IER_RX (1 << 0)
#define UART_IER_TX (1 << 1)
#define UART_MCR_OUT2 (1 << 3)
#define UART_LSR_DR (1 << 0)
#define UART_LSR_THRE (1 << 5)
#define UART_LSR_TEMT (1 << 6)

#define PLIC_CONTEXT(hart) ((hart) * 2 + 1) /* S-mode context */

static unsigned long plic_base = 0;

static inline unsigned long plic_priority_addr(int irq)
{
    return plic_base + (unsigned long)irq * 4UL;
}

static inline unsigned long plic_enable_addr(int ctx)
{
    return plic_base + 0x2000UL + (unsigned long)ctx * 0x80UL;
}

static inline unsigned long plic_threshold_addr(int ctx)
{
    return plic_base + 0x200000UL + (unsigned long)ctx * 0x1000UL;
}

static inline unsigned long plic_claim_addr(int ctx)
{
    return plic_base + 0x200004UL + (unsigned long)ctx * 0x1000UL;
}

static unsigned long boot_cpu_hartid = 0;
static int uart_irq_id = 42;

static inline void write32(unsigned long addr, unsigned int value)
{
    *(volatile unsigned int *)addr = value;
}

static inline unsigned int read32(unsigned long addr)
{
    return *(volatile unsigned int *)addr;
}

static unsigned int uart_ier_shadow;

static void uart_enable_tx_irq(void)
{
    uart_ier_shadow |= UART_IER_RX | UART_IER_TX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void uart_disable_tx_irq(void)
{
    uart_ier_shadow &= ~UART_IER_TX;
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}
/* ---------- CSR / trap constants ---------- */
#define SSTATUS_SIE (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP (1UL << 8)

#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

#define SCAUSE_INTERRUPT (1UL << 63)
#define SCAUSE_U_ECALL 8UL
#define SCAUSE_S_TIMER (SCAUSE_INTERRUPT | 5UL)
#define SCAUSE_S_EXT (SCAUSE_INTERRUPT | 9UL)

struct pt_regs
{
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
#define SBI_EXT_SHUTDOWN 0x8
#define SBI_EXT_BASE 0x10

struct sbiret
{
    long error;
    long value;
};

static struct sbiret sbi_ecall(int ext, int fid,
                               unsigned long arg0,
                               unsigned long arg1,
                               unsigned long arg2,
                               unsigned long arg3,
                               unsigned long arg4,
                               unsigned long arg5)
{
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

static void sbi_set_timer(unsigned long stime_value)
{
    sbi_ecall(SBI_EXT_SET_TIMER, 0, stime_value, 0, 0, 0, 0, 0);
}

static unsigned long sbi_get_spec_version(void)
{
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 0, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_id(void)
{
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 1, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_version(void)
{
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 2, 0, 0, 0, 0, 0, 0).value;
}

static inline unsigned long read_time(void)
{
    unsigned long x;
    asm volatile("rdtime %0" : "=r"(x));
    return x;
}

static inline void local_irq_enable(void)
{
    asm volatile("csrsi sstatus, 2" ::: "memory");
}

static inline void local_irq_disable(void)
{
    asm volatile("csrci sstatus, 2" ::: "memory");
}

static void enable_timer_interrupt(void)
{
    asm volatile("csrs sie, %0" ::"r"(SIE_STIE) : "memory");
}

static void enable_external_interrupt(void)
{
    asm volatile("csrs sie, %0" ::"r"(SIE_SEIE) : "memory");
}

static inline void set_stvec_relocated(void)
{
    unsigned long relocated_trap =
        RELOC_ADDR + ((unsigned long)handle_exception - (unsigned long)_start);
    asm volatile("csrw stvec, %0" ::"r"(relocated_trap) : "memory");
}

static inline unsigned long irq_save(void)
{
    unsigned long flags;
    asm volatile("csrrci %0, sstatus, 2" : "=r"(flags)::"memory");
    return flags;
}

static inline void irq_restore(unsigned long flags)
{
    if (flags & SSTATUS_SIE)
        local_irq_enable();
    else
        local_irq_disable();
}

/* ---------- tiny libc ---------- */
static size_t strlen_simple(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int strcmp_full(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strcmp_simple(const char *a, const char *b)
{
    return strcmp_full(a, b) == 0;
}

static int strncmp_simple(const char *a, const char *b, size_t n)
{
    while (n-- > 0)
    {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
    }
    return 0;
}

int strncmp(const char *a, const char *b, int n)
{
    if (n <= 0)
        return 0;
    return strncmp_simple(a, b, (size_t)n);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];

    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)value;

    return dst;
}

static void strcpy_simple(char *dst, const char *src)
{
    while ((*dst++ = *src++))
        ;
}

static void strncpy_message(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    if (max == 0)
        return;
    while (i + 1 < max && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void strcat_simple(char *dst, const char *src)
{
    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
}

static int memcmp_simple(const void *s1, const void *s2, int n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    while (n-- > 0)
    {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
    }
    return 0;
}

static int hextoi_simple(const char *s, int n)
{
    int r = 0;
    while (n-- > 0)
    {
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

static int align_int(int n, int byte)
{
    return (n + byte - 1) & ~(byte - 1);
}

static const void *align_up_ptr(const void *ptr, size_t align)
{
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static void uart_put_uint(unsigned long x)
{
    char buf[32];
    int i = 0;

    if (x == 0)
    {
        uart_putc('0');
        return;
    }

    while (x > 0)
    {
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
static void trap_puts(const char *s)
{
    uart_puts_polling(s);
}

static void trap_put_uint(unsigned long x)
{
    char buf[32];
    int i = 0;

    if (x == 0)
    {
        uart_putc_polling('0');
        return;
    }

    while (x > 0)
    {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0)
        uart_putc_polling(buf[--i]);
}

static void trap_hex(unsigned long h)
{
    const char *hex = "0123456789abcdef";

    trap_puts("0x");
    for (int i = (int)(sizeof(unsigned long) * 2) - 1; i >= 0; i--)
        uart_putc_polling(hex[(h >> (i * 4)) & 0xf]);
}

static void trap_hex32(unsigned long h)
{
    const char *hex = "0123456789abcdef";

    trap_puts("0x");
    for (int i = 7; i >= 0; i--)
        uart_putc_polling(hex[(h >> (i * 4)) & 0xf]);
}

/* ---------- FDT parser ---------- */
#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE 0x00000002
#define FDT_PROP 0x00000003
#define FDT_NOP 0x00000004
#define FDT_END 0x00000009

struct fdt_header
{
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

static uint32_t bswap32_main(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static unsigned long read_cells(const uint32_t *p, int cells)
{
    unsigned long v = 0;
    for (int i = 0; i < cells; i++)
        v = (v << 32) | bswap32_main(p[i]);
    return v;
}

static int fdt_is_valid(const void *fdt)
{
    if (!fdt)
        return 0;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    return bswap32_main(hdr->magic) == 0xd00dfeed;
}

static int bytes_contains_string(const char *buf, int len, const char *needle)
{
    size_t nlen = strlen_simple(needle);

    if (!buf || !needle || nlen == 0 || len <= 0)
        return 0;

    for (int i = 0; i + (int)nlen <= len; i++)
    {
        size_t j = 0;
        while (j < nlen && buf[i + (int)j] == needle[j])
            j++;
        if (j == nlen)
            return 1;
    }

    return 0;
}

int fdt_path_offset(const void *fdt, const char *path)
{
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (bswap32_main(hdr->magic) != 0xd00dfeed)
        return -1;

    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *p = struct_base;
    char curpath[1024];
    size_t pathlen_stack[128];
    int depth = 0;

    curpath[0] = '\0';

    while (1)
    {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE)
        {
            const char *name = p;
            size_t oldlen = strlen_simple(curpath);
            pathlen_stack[depth++] = oldlen;

            if (oldlen == 0)
            {
                strcpy_simple(curpath, "/");
            }
            else
            {
                if (strcmp_full(curpath, "/") != 0)
                    strcat_simple(curpath, "/");
                strcat_simple(curpath, name);
            }

            const char *a = curpath;
            const char *b = path;
            int matched = 1;

            while (*a || *b)
            {
                if (*a == '/' && *b == '/')
                {
                    a++;
                    b++;
                    continue;
                }

                while (*a && *b && *a != '/' && *b != '/' && *a == *b)
                {
                    a++;
                    b++;
                }

                if (!((*b == '\0' || *b == '/') &&
                      (*a == '\0' || *a == '/' || *a == '@')))
                {
                    matched = 0;
                    break;
                }

                while (*a && *a != '/')
                    a++;
                while (*b && *b != '/')
                    b++;

                if ((*a == '\0') != (*b == '\0'))
                {
                    matched = 0;
                    break;
                }
            }

            if (matched)
                return nodeoff;

            p = (const char *)align_up_ptr(p + strlen_simple(name) + 1, 4);
        }
        else if (tag == FDT_END_NODE)
        {
            if (depth > 0)
            {
                size_t oldlen = pathlen_stack[--depth];
                curpath[oldlen] = '\0';
            }
        }
        else if (tag == FDT_PROP)
        {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 8;
            p = (const char *)align_up_ptr(p + len, 4);
        }
        else if (tag == FDT_NOP)
        {
        }
        else if (tag == FDT_END)
        {
            break;
        }
        else
        {
            return -1;
        }
    }

    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset,
                        const char *name, int *lenp)
{
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

    while (1)
    {
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_PROP)
        {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 4;
            uint32_t nameoff = bswap32_main(*(const uint32_t *)p);
            p += 4;

            const char *prop_name = strings_base + nameoff;
            const void *prop_data = p;

            if (depth == 0 && strcmp_full(prop_name, name) == 0)
            {
                if (lenp)
                    *lenp = (int)len;
                return prop_data;
            }

            p = (const char *)align_up_ptr(p + len, 4);
        }
        else if (tag == FDT_BEGIN_NODE)
        {
            depth++;
            p = (const char *)align_up_ptr(p + strlen_simple(p) + 1, 4);
        }
        else if (tag == FDT_END_NODE)
        {
            if (depth == 0)
                break;
            depth--;
        }
        else if (tag == FDT_NOP)
        {
        }
        else if (tag == FDT_END)
        {
            break;
        }
        else
        {
            return 0;
        }
    }

    return 0;
}

static void fdt_write_u64_prop(void *fdt, int node, const char *name,
                               unsigned long value)
{
    int len = 0;
    uint32_t *prop = (uint32_t *)fdt_getprop(fdt, node, name, &len);

    if (!prop || len < 8)
    {
        uart_puts("fdt prop missing: ");
        uart_puts(name);
        uart_puts("\n");
        return;
    }

    prop[0] = bswap32_main((uint32_t)(value >> 32));
    prop[1] = bswap32_main((uint32_t)(value & 0xffffffffUL));
}

static void *make_writable_fdt_copy(const void *old_fdt)
{
    if (!fdt_is_valid(old_fdt))
    {
        uart_puts("invalid fdt\n");
        return 0;
    }

    const struct fdt_header *old_hdr = (const struct fdt_header *)old_fdt;
    unsigned int old_size = bswap32_main(old_hdr->totalsize);

    if (old_size > NEW_FDT_SIZE)
    {
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
                                 unsigned long initrd_end_addr)
{
    int chosen = fdt_path_offset(fdt, "/chosen");

    if (chosen < 0)
    {
        uart_puts("/chosen not found\n");
        return;
    }

    fdt_write_u64_prop(fdt, chosen, "linux,initrd-start", initrd_start_addr);
    fdt_write_u64_prop(fdt, chosen, "linux,initrd-end", initrd_end_addr);
}

static const void *initrd_start = 0;
static const void *initrd_end = 0;
static unsigned long timebase_frequency = 10000000UL;

static void initrd_init_from_dtb(const void *fdt)
{
    int offset = fdt_path_offset(fdt, "/chosen");
    int len;

    if (offset < 0)
    {
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

static int fdt_find_uart_node(const void *fdt)
{
    if (!fdt_is_valid(fdt))
        return -1;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *p = struct_base;

    while (1)
    {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE)
        {
            const char *name = p;
            int len = 0;
            const char *compat = (const char *)fdt_getprop(fdt, nodeoff,
                                                           "compatible", &len);

            if ((compat &&
                 (bytes_contains_string(compat, len, "ns16550a") ||
                  bytes_contains_string(compat, len, "ns16550") ||
                  bytes_contains_string(compat, len, "uart"))) ||
                strncmp_simple(name, "serial", 6) == 0 ||
                strncmp_simple(name, "uart", 4) == 0)
            {
                return nodeoff;
            }

            p = (const char *)align_up_ptr(p + strlen_simple(name) + 1, 4);
        }
        else if (tag == FDT_PROP)
        {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 8;
            p = (const char *)align_up_ptr(p + len, 4);
        }
        else if (tag == FDT_END_NODE)
        {
        }
        else if (tag == FDT_NOP)
        {
        }
        else if (tag == FDT_END)
        {
            break;
        }
        else
        {
            return -1;
        }
    }

    return -1;
}

static void uart_init_from_dtb(const void *fdt)
{
    int len;
    unsigned long uart_base = 0x10000000UL;
    int reg_shift = 0;
    int reg_width = 1;

    int node = fdt_path_offset(fdt, "/soc/serial@d4017000");
    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/serial@10000000");
    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/serial");
    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/uart");
    if (node < 0)
        node = fdt_find_uart_node(fdt);

    if (node < 0)
    {
        uart_set_base(uart_base);
        uart_set_config(reg_shift, reg_width);
        uart_irq_id = 10;
    }
    else
    {
        const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
        if (reg && len >= 16)
            uart_base = read_cells(reg, 2);
        else if (reg && len >= 8)
            uart_base = read_cells(reg, 1);

        const char *compat = (const char *)fdt_getprop(fdt, node, "compatible", &len);
        if (compat &&
            !(bytes_contains_string(compat, len, "ns16550") ||
              bytes_contains_string(compat, len, "ns16550a")))
        {
            reg_shift = 2;
            reg_width = 4;
        }

        int shift_len = 0;
        int width_len = 0;
        const uint32_t *shift = (const uint32_t *)fdt_getprop(fdt, node, "reg-shift", &shift_len);
        const uint32_t *width = (const uint32_t *)fdt_getprop(fdt, node, "reg-io-width", &width_len);
        reg_shift = (shift && shift_len >= 4) ? (int)bswap32_main(shift[0]) : reg_shift;
        reg_width = (width && width_len >= 4) ? (int)bswap32_main(width[0]) : reg_width;

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

static int fdt_find_plic_node(const void *fdt)
{
    if (!fdt_is_valid(fdt))
        return -1;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    const char *struct_base = (const char *)fdt + bswap32_main(hdr->off_dt_struct);
    const char *p = struct_base;

    while (1)
    {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32_main(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE)
        {
            const char *name = p;
            int len = 0;
            const char *compat = (const char *)fdt_getprop(fdt, nodeoff, "compatible", &len);
            const void *ndev = fdt_getprop(fdt, nodeoff, "riscv,ndev", 0);

            if ((compat &&
                 (bytes_contains_string(compat, len, "riscv,plic0") ||
                  bytes_contains_string(compat, len, "sifive,plic-1.0.0") ||
                  bytes_contains_string(compat, len, "plic"))) ||
                ndev)
            {
                return nodeoff;
            }

            p = (const char *)align_up_ptr(p + strlen_simple(name) + 1, 4);
        }
        else if (tag == FDT_PROP)
        {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 8;
            p = (const char *)align_up_ptr(p + len, 4);
        }
        else if (tag == FDT_END_NODE)
        {
        }
        else if (tag == FDT_NOP)
        {
        }
        else if (tag == FDT_END)
        {
            break;
        }
        else
        {
            return -1;
        }
    }

    return -1;
}

static void plic_init_from_dtb(const void *fdt)
{
    int len = 0;
    int node = fdt_find_plic_node(fdt);

    if (node < 0)
    {
        return;
    }

    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
    if (!reg || len < 8)
    {
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

static void timer_frequency_init_from_dtb(const void *fdt)
{
    int cpus = fdt_path_offset(fdt, "/cpus");
    if (cpus < 0)
    {
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
struct cpio_t
{
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
static void console_flush_async(void);
static void uart_pump_tx(void);
static void uart_pump_tx_aggressive(void);
static int console_tx_pending(void);

static void initrd_list(const void *rd)
{
    const char *p = (const char *)rd;

    while (p && p < (const char *)initrd_end)
    {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (memcmp_simple(hdr->magic, "070701", 6) != 0)
        {
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

static const void *initrd_find(const char *filename, int *filesize_out)
{
    const char *p = (const char *)initrd_start;

    while (p && p < (const char *)initrd_end)
    {
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

        if (strcmp_full(cmp_name, filename) == 0)
        {
            if (filesize_out)
                *filesize_out = filesize;
            return data;
        }

        p = data + align_int(filesize, 4);
    }

    return 0;
}

static void initrd_cat(const void *rd, const char *filename)
{
    (void)rd;
    int size = 0;
    const char *data = (const char *)initrd_find(filename, &size);

    if (!data)
    {
        console_puts_async("initrd_cat: ");
        console_puts_async(filename);
        console_puts_async(": No such file\n");
        return;
    }

    for (int i = 0; i < size; i++)
        console_putc_async(data[i]);
    console_putc_async('\n');
}

/* ---------- Lab5 thread / process scheduler ---------- */
struct thread_context
{
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
};

enum task_state
{
    TASK_RUNNING = 0,
    TASK_SLEEPING,
    TASK_WAITING,
    TASK_ZOMBIE,
};

enum task_kind
{
    TASK_KERNEL = 0,
    TASK_USER,
};

typedef void (*kernel_thread_fn)(void *arg);

#define THREAD_STACK_SIZE (16 * 1024UL)
#define USER_STACK_SIZE (64 * 1024UL)
#define MAX_SIGNAL 32
#define SIGTERM 15

struct task_struct
{
    struct thread_context thread;
    int pid;
    int state;
    int kind;
    int exit_status;
    long waiting_pid;
    unsigned long wakeup_tick;
    void *kernel_stack;
    void *user_stack;
    unsigned long user_stack_size;
    void *user_image;
    unsigned long user_image_size;
    int legacy_ecall_demo;
    int legacy_ecall_count;
    struct pt_regs *trap_frame;
    kernel_thread_fn kernel_entry;
    void *kernel_arg;
    struct task_struct *parent;
    struct task_struct *next;
    unsigned long signal_handlers[MAX_SIGNAL];
    unsigned long pending_signals;
    int in_signal;
    struct pt_regs saved_signal_frame;
    void *signal_stack;
};

static struct task_struct boot_task;
static struct task_struct *run_queue = 0;
static struct task_struct *current_task = 0;
static int next_pid = 1;
static int scheduler_ready = 0;

static void schedule(void);
static void kill_zombies(void);
static void process_wake_sleepers(void);
static int exec_user_program(const char *filename);
static void run_tasks(void);

static struct task_struct *get_current(void)
{
    return current_task;
}

static unsigned long task_ticks_per_sec(void)
{
    return timebase_frequency ? timebase_frequency : 10000000UL;
}

static void enqueue_task(struct task_struct *task)
{
    if (!run_queue)
    {
        run_queue = task;
        task->next = task;
        return;
    }

    struct task_struct *tail = run_queue;
    while (tail->next != run_queue)
        tail = tail->next;

    tail->next = task;
    task->next = run_queue;
}

static void remove_task_from_queue(struct task_struct *task)
{
    if (!run_queue || !task)
        return;

    struct task_struct *prev = run_queue;
    while (prev->next != task && prev->next != run_queue)
        prev = prev->next;

    if (prev->next != task)
        return;

    if (task->next == task)
    {
        run_queue = 0;
    }
    else
    {
        prev->next = task->next;
        if (run_queue == task)
            run_queue = task->next;
    }

    task->next = 0;
}

static struct task_struct *find_task(int pid)
{
    if (!run_queue)
        return 0;

    struct task_struct *cur = run_queue;
    do
    {
        if (cur->pid == pid)
            return cur;
        cur = cur->next;
    } while (cur != run_queue);

    return 0;
}

static int task_has_child(struct task_struct *parent, long pid)
{
    if (!run_queue)
        return 0;

    struct task_struct *cur = run_queue;
    do
    {
        if (cur->parent == parent && (pid < 0 || cur->pid == pid))
            return 1;
        cur = cur->next;
    } while (cur != run_queue);

    return 0;
}

static void release_task(struct task_struct *task)
{
    if (!task || task == &boot_task || task == get_current())
        return;

    remove_task_from_queue(task);

    if (task->signal_stack)
        free(task->signal_stack);
    if (task->user_stack)
        free(task->user_stack);
    if (task->kernel_stack)
        free(task->kernel_stack);

    free(task);
}

static void wake_waiters(int pid)
{
    if (!run_queue)
        return;

    struct task_struct *cur = run_queue;
    do
    {
        if (cur->state == TASK_WAITING &&
            (cur->waiting_pid < 0 || cur->waiting_pid == pid))
        {
            cur->state = TASK_RUNNING;
        }
        cur = cur->next;
    } while (cur != run_queue);
}

static void mark_task_zombie(struct task_struct *task, int status)
{
    if (!task || task->state == TASK_ZOMBIE)
        return;

    task->exit_status = status;
    task->state = TASK_ZOMBIE;
    wake_waiters(task->pid);
}

static struct task_struct *pick_next_task(struct task_struct *prev)
{
    if (!run_queue)
        return 0;

    struct task_struct *cur = prev && prev->next ? prev->next : run_queue;
    struct task_struct *start = cur;

    do
    {
        if (cur->state == TASK_RUNNING)
            return cur;
        cur = cur->next;
    } while (cur != start);

    return prev && prev->state == TASK_RUNNING ? prev : 0;
}

static void schedule(void)
{
    if (!scheduler_ready || !run_queue)
        return;

    struct task_struct *prev = get_current();
    struct task_struct *next = pick_next_task(prev);

    if (!next || next == prev)
        return;

    irq_save();
    current_task = next;
    switch_to(prev, next);

    /*
     * switch_to() only preserves general registers.  User mode owns tp, so
     * kernel current tracking lives in current_task instead.  If a timer trap
     * switches from one kernel thread to another, the resumed thread inherits
     * trap-time SIE=0 unless we restore the kernel scheduling invariant here.
     */
    if (get_current()->kind == TASK_KERNEL)
        local_irq_enable();
}

static void kernel_thread_start(void)
{
    struct task_struct *current = get_current();

    local_irq_enable();
    current->kernel_entry(current->kernel_arg);
    local_irq_disable();

    mark_task_zombie(current, 0);
    schedule();

    while (1)
        ;
}

static struct task_struct *create_kernel_thread(kernel_thread_fn fn, void *arg)
{
    struct task_struct *task = (struct task_struct *)allocate(sizeof(*task));
    void *stack = allocate(THREAD_STACK_SIZE);

    if (!task || !stack)
    {
        if (task)
            free(task);
        if (stack)
            free(stack);
        return 0;
    }

    memset(task, 0, sizeof(*task));
    task->pid = next_pid++;
    task->state = TASK_RUNNING;
    task->kind = TASK_KERNEL;
    task->kernel_stack = stack;
    task->kernel_entry = fn;
    task->kernel_arg = arg;
    task->waiting_pid = -1;
    task->thread.ra = (unsigned long)kernel_thread_start;
    task->thread.sp = ((unsigned long)stack + THREAD_STACK_SIZE) & ~0xFUL;

    enqueue_task(task);
    return task;
}

static void user_process_start(void)
{
    struct task_struct *current = get_current();

    current->trap_frame->sstatus &= ~SSTATUS_SPP;
    current->trap_frame->sstatus |= SSTATUS_SPIE;
    restore_trap_frame(current->trap_frame);

    while (1)
        ;
}

static int is_legacy_ecall_demo_program(const char *filename,
                                        const char *data,
                                        int filesize)
{
    static const unsigned char legacy_prog[] = {
        0x95, 0x48, 0x73, 0x00, 0x00, 0x00, 0xfd,
        0x18, 0xe3, 0x1d, 0x10, 0xff, 0x01, 0xa0,
    };

    if (strcmp_full(filename, "prog.bin") != 0)
        return 0;
    if (filesize != (int)sizeof(legacy_prog))
        return 0;

    return memcmp_simple(data, legacy_prog, (int)sizeof(legacy_prog)) == 0;
}

static struct task_struct *create_user_process(const char *filename,
                                               struct task_struct *parent)
{
    int filesize = 0;
    const char *data = (const char *)initrd_find(filename, &filesize);

    if (!data)
        return 0;

    struct task_struct *task = (struct task_struct *)allocate(sizeof(*task));
    void *kstack = allocate(THREAD_STACK_SIZE);
    void *ustack = allocate(USER_STACK_SIZE);

    if (!task || !kstack || !ustack)
    {
        if (task)
            free(task);
        if (kstack)
            free(kstack);
        if (ustack)
            free(ustack);
        return 0;
    }

    memset(task, 0, sizeof(*task));
    task->pid = next_pid++;
    task->state = TASK_RUNNING;
    task->kind = TASK_USER;
    task->parent = parent;
    task->kernel_stack = kstack;
    task->user_stack = ustack;
    task->user_stack_size = USER_STACK_SIZE;
    task->user_image = (void *)data;
    task->user_image_size = (unsigned long)filesize;
    task->legacy_ecall_demo =
        is_legacy_ecall_demo_program(filename, data, filesize);
    task->legacy_ecall_count = 0;
    task->waiting_pid = -1;

    struct pt_regs *tf =
        (struct pt_regs *)((unsigned long)kstack + THREAD_STACK_SIZE -
                           sizeof(struct pt_regs));
    tf = (struct pt_regs *)((unsigned long)tf & ~0xFUL);
    memset(tf, 0, sizeof(*tf));
    tf->sepc = (unsigned long)data;
    tf->sp = (unsigned long)ustack + USER_STACK_SIZE;
    tf->sstatus = SSTATUS_SPIE;

    task->trap_frame = tf;
    task->thread.ra = (unsigned long)user_process_start;
    task->thread.sp = (unsigned long)tf;

    (void)filesize;
    enqueue_task(task);
    return task;
}

static void adjust_stack_reg(unsigned long *reg,
                             unsigned long old_base,
                             unsigned long old_end,
                             long delta)
{
    if (*reg >= old_base && *reg < old_end)
        *reg = (unsigned long)((long)(*reg) + delta);
}

static void adjust_child_stack_regs(struct pt_regs *tf,
                                    unsigned long old_base,
                                    unsigned long old_end,
                                    long delta)
{
    adjust_stack_reg(&tf->ra, old_base, old_end, delta);
    adjust_stack_reg(&tf->sp, old_base, old_end, delta);
    adjust_stack_reg(&tf->gp, old_base, old_end, delta);
    adjust_stack_reg(&tf->tp, old_base, old_end, delta);
    adjust_stack_reg(&tf->t0, old_base, old_end, delta);
    adjust_stack_reg(&tf->t1, old_base, old_end, delta);
    adjust_stack_reg(&tf->t2, old_base, old_end, delta);
    adjust_stack_reg(&tf->s0, old_base, old_end, delta);
    adjust_stack_reg(&tf->s1, old_base, old_end, delta);
    adjust_stack_reg(&tf->a0, old_base, old_end, delta);
    adjust_stack_reg(&tf->a1, old_base, old_end, delta);
    adjust_stack_reg(&tf->a2, old_base, old_end, delta);
    adjust_stack_reg(&tf->a3, old_base, old_end, delta);
    adjust_stack_reg(&tf->a4, old_base, old_end, delta);
    adjust_stack_reg(&tf->a5, old_base, old_end, delta);
    adjust_stack_reg(&tf->a6, old_base, old_end, delta);
    adjust_stack_reg(&tf->a7, old_base, old_end, delta);
    adjust_stack_reg(&tf->s2, old_base, old_end, delta);
    adjust_stack_reg(&tf->s3, old_base, old_end, delta);
    adjust_stack_reg(&tf->s4, old_base, old_end, delta);
    adjust_stack_reg(&tf->s5, old_base, old_end, delta);
    adjust_stack_reg(&tf->s6, old_base, old_end, delta);
    adjust_stack_reg(&tf->s7, old_base, old_end, delta);
    adjust_stack_reg(&tf->s8, old_base, old_end, delta);
    adjust_stack_reg(&tf->s9, old_base, old_end, delta);
    adjust_stack_reg(&tf->s10, old_base, old_end, delta);
    adjust_stack_reg(&tf->s11, old_base, old_end, delta);
    adjust_stack_reg(&tf->t3, old_base, old_end, delta);
    adjust_stack_reg(&tf->t4, old_base, old_end, delta);
    adjust_stack_reg(&tf->t5, old_base, old_end, delta);
    adjust_stack_reg(&tf->t6, old_base, old_end, delta);
}

static long fork_current_process(struct pt_regs *regs)
{
    struct task_struct *parent = get_current();

    if (!parent || parent->kind != TASK_USER)
        return -1;

    struct task_struct *child = (struct task_struct *)allocate(sizeof(*child));
    void *kstack = allocate(THREAD_STACK_SIZE);
    void *ustack = allocate(USER_STACK_SIZE);

    if (!child || !kstack || !ustack)
    {
        if (child)
            free(child);
        if (kstack)
            free(kstack);
        if (ustack)
            free(ustack);
        return -1;
    }

    memset(child, 0, sizeof(*child));
    child->pid = next_pid++;
    child->state = TASK_RUNNING;
    child->kind = TASK_USER;
    child->parent = parent;
    child->kernel_stack = kstack;
    child->user_stack = ustack;
    child->user_stack_size = USER_STACK_SIZE;
    child->user_image = parent->user_image;
    child->user_image_size = parent->user_image_size;
    child->legacy_ecall_demo = parent->legacy_ecall_demo;
    child->legacy_ecall_count = parent->legacy_ecall_count;
    child->waiting_pid = -1;

    memcpy(child->signal_handlers, parent->signal_handlers,
           sizeof(child->signal_handlers));

    memcpy(ustack, parent->user_stack, USER_STACK_SIZE);

    struct pt_regs *child_tf =
        (struct pt_regs *)((unsigned long)kstack + THREAD_STACK_SIZE -
                           sizeof(struct pt_regs));
    child_tf = (struct pt_regs *)((unsigned long)child_tf & ~0xFUL);
    memcpy(child_tf, regs, sizeof(*child_tf));
    child_tf->sepc = regs->sepc + 4;
    child_tf->a0 = 0;

    unsigned long old_base = (unsigned long)parent->user_stack;
    unsigned long old_end = old_base + parent->user_stack_size;
    long delta = (long)((unsigned long)ustack - old_base);
    adjust_child_stack_regs(child_tf, old_base, old_end, delta);

    child->trap_frame = child_tf;
    child->thread.ra = (unsigned long)user_process_start;
    child->thread.sp = (unsigned long)child_tf;

    enqueue_task(child);
    return child->pid;
}

static void user_console_release_current(void);

static long wait_for_child(long pid)
{
    struct task_struct *current = get_current();

    user_console_release_current();

    while (1)
    {
        if (!task_has_child(current, pid))
            return -1;

        if (run_queue)
        {
            struct task_struct *cur = run_queue;
            do
            {
                struct task_struct *next = cur->next;
                if (cur->parent == current &&
                    (pid < 0 || cur->pid == pid) &&
                    cur->state == TASK_ZOMBIE)
                {
                    int done_pid = cur->pid;
                    release_task(cur);
                    return done_pid;
                }
                cur = next;
            } while (run_queue && cur != run_queue);
        }

        current->waiting_pid = pid;
        current->state = TASK_WAITING;
        schedule();
        current->state = TASK_RUNNING;
        current->waiting_pid = -1;
    }
}

static int user_console_owner_pid = -1;

static void user_console_release_pid(int pid)
{
    if (user_console_owner_pid == pid)
        user_console_owner_pid = -1;
}

static void user_console_release_current(void)
{
    struct task_struct *current = get_current();

    if (current)
        user_console_release_pid(current->pid);
}

static void user_console_wait_turn(struct task_struct *current)
{
    if (!current || current->kind != TASK_USER)
        return;

    while (user_console_owner_pid >= 0 &&
           user_console_owner_pid != current->pid)
    {
        struct task_struct *owner = find_task(user_console_owner_pid);

        if (!owner || owner->state == TASK_ZOMBIE)
        {
            user_console_owner_pid = -1;
            break;
        }

        schedule();
    }

    user_console_owner_pid = current->pid;
}

static void process_exit_current(int status)
{
    struct task_struct *current = get_current();

    user_console_release_current();
    mark_task_zombie(current, status);
    schedule();

    while (1)
        ;
}

static int stop_task(long pid)
{
    struct task_struct *task = find_task((int)pid);

    if (!task || task == &boot_task)
        return -1;

    user_console_release_pid(task->pid);
    mark_task_zombie(task, -1);

    if (task == get_current())
        process_exit_current(-1);

    return 0;
}

static void process_sleep_usec(unsigned int usec)
{
    struct task_struct *current = get_current();
    unsigned long ticks =
        ((unsigned long)usec * task_ticks_per_sec() + 999999UL) / 1000000UL;

    user_console_release_current();
    current->wakeup_tick = read_time() + ticks;
    current->state = TASK_SLEEPING;
    schedule();
    current->state = TASK_RUNNING;
    current->wakeup_tick = 0;
}

static void process_wake_sleepers(void)
{
    if (!run_queue)
        return;

    unsigned long now = read_time();
    struct task_struct *cur = run_queue;

    do
    {
        if (cur->state == TASK_SLEEPING &&
            cur->wakeup_tick && now >= cur->wakeup_tick)
        {
            cur->state = TASK_RUNNING;
            cur->wakeup_tick = 0;
        }
        cur = cur->next;
    } while (cur != run_queue);
}

static void kill_zombies(void)
{
    if (!run_queue)
        return;

    struct task_struct *cur = run_queue;
    do
    {
        struct task_struct *next = cur->next;
        if (cur != get_current() && cur->state == TASK_ZOMBIE &&
            (!cur->parent || cur->parent->state == TASK_ZOMBIE) &&
            !task_has_child(cur, -1))
        {
            release_task(cur);
        }
        cur = next;
    } while (run_queue && cur != run_queue);
}

static void idle_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        kill_zombies();
        uart_pump_tx();
        schedule();
        if (console_tx_pending())
            continue;
        local_irq_enable();
        asm volatile("wfi");
    }
}

static int process_exec_foreground(const char *filename)
{
    struct task_struct *child = create_user_process(filename, get_current());

    if (!child)
    {
        console_puts_async("Failed to exec user program!\n");
        return -1;
    }

    return (int)wait_for_child(child->pid);
}

static int exec_user_program(const char *filename)
{
    return process_exec_foreground(filename);
}

static int range_contains(unsigned long base, unsigned long size,
                          unsigned long addr, unsigned long len)
{
    if (len == 0)
        return 1;
    if (!base || size < len || addr < base)
        return 0;

    return addr - base <= size - len;
}

static int user_range_ok(const void *ptr, unsigned long len)
{
    struct task_struct *current = get_current();
    unsigned long addr = (unsigned long)ptr;

    if (len == 0)
        return 1;
    if (!ptr || !current || current->kind != TASK_USER)
        return 0;

    if (range_contains((unsigned long)current->user_stack,
                       current->user_stack_size, addr, len))
        return 1;
    if (range_contains((unsigned long)current->user_image,
                       current->user_image_size, addr, len))
        return 1;
    if (range_contains((unsigned long)current->signal_stack,
                       current->signal_stack ? PAGE_SIZE : 0, addr, len))
        return 1;

    return 0;
}

static int copy_user_string(char *dst, const char *src, size_t max)
{
    unsigned long base = (unsigned long)src;

    if (!dst || !src || max == 0)
        return -1;

    for (size_t i = 0; i + 1 < max; i++)
    {
        unsigned long addr = base + i;
        const char *p;

        if (addr < base)
            return -1;

        p = (const char *)addr;
        if (!user_range_ok(p, 1))
            return -1;

        dst[i] = *p;
        if (dst[i] == '\0')
            return 0;
    }

    dst[max - 1] = '\0';
    return -1;
}

enum
{
    SYS_GETPID = 0,
    SYS_UART_READ = 1,
    SYS_UART_WRITE = 2,
    SYS_EXEC = 3,
    SYS_FORK = 4,
    SYS_WAITPID = 5,
    SYS_EXIT = 6,
    SYS_STOP = 7,
    SYS_DISPLAY = 8,
    SYS_USLEEP = 9,
    SYS_SIGNAL = 10,
    SYS_SIGRETURN = 11,
    SYS_KILL = 12,
    SYS_LEGACY_EXIT = 93,
};

static long sys_uart_read(char *buf, long count)
{
    user_console_release_current();

    if (count < 0)
        return -1;
    if (count == 0)
        return 0;
    if (!user_range_ok(buf, (unsigned long)count))
        return -1;

    for (long i = 0; i < count; i++)
        buf[i] = uart_getc();

    return count;
}

static long sys_uart_write(const char *buf, long count)
{
    struct task_struct *current = get_current();

    if (count < 0)
        return -1;
    if (count == 0)
        return 0;
    if (!user_range_ok(buf, (unsigned long)count))
        return -1;

    for (long i = 0; i < count; i++)
    {
        user_console_wait_turn(current);
        uart_putc(buf[i]);

        if (buf[i] == '\n' || buf[i] == '\r')
            user_console_release_current();
    }

    return count;
}

static int sys_exec(struct pt_regs *regs, const char *path)
{
    char filename[128];
    int filesize = 0;
    const char *data;
    struct task_struct *current = get_current();

    if (copy_user_string(filename, path, sizeof(filename)) < 0)
        return -1;

    data = (const char *)initrd_find(filename, &filesize);
    if (!data)
        return -1;

    void *new_stack = allocate(USER_STACK_SIZE);
    if (!new_stack)
        return -1;

    if (current->user_stack)
        free(current->user_stack);

    current->user_stack = new_stack;
    current->user_stack_size = USER_STACK_SIZE;
    current->user_image = (void *)data;
    current->user_image_size = (unsigned long)filesize;
    current->legacy_ecall_demo =
        is_legacy_ecall_demo_program(filename, data, filesize);
    current->legacy_ecall_count = 0;

    memset(regs, 0, sizeof(*regs));
    regs->sepc = (unsigned long)data;
    regs->sp = (unsigned long)new_stack + USER_STACK_SIZE;
    regs->sstatus = SSTATUS_SPIE;
    current->trap_frame = regs;

    (void)filesize;
    return 0;
}

static int first_pending_signal(struct task_struct *task)
{
    for (int i = 1; i < MAX_SIGNAL; i++)
    {
        if (task->pending_signals & (1UL << i))
            return i;
    }

    return 0;
}

static void deliver_signal_if_needed(struct pt_regs *regs)
{
    struct task_struct *current = get_current();

    if (!current || current->kind != TASK_USER || current->in_signal)
        return;
    if (regs->sstatus & SSTATUS_SPP)
        return;

    int signum = first_pending_signal(current);
    if (!signum)
        return;

    current->pending_signals &= ~(1UL << signum);

    unsigned long handler = current->signal_handlers[signum];
    if (!handler)
    {
        process_exit_current(128 + signum);
        return;
    }

    void *sig_stack = allocate(PAGE_SIZE);
    if (!sig_stack)
    {
        process_exit_current(128 + signum);
        return;
    }

    current->saved_signal_frame = *regs;
    current->signal_stack = sig_stack;
    current->in_signal = 1;

    unsigned long top = ((unsigned long)sig_stack + PAGE_SIZE) & ~0xFUL;
    unsigned int *trampoline = (unsigned int *)(top - 8);
    trampoline[0] = 0x00b00893U; /* addi a7, zero, 11 */
    trampoline[1] = 0x00000073U; /* ecall */
    asm volatile("fence.i" ::: "memory");

    regs->sepc = handler;
    regs->ra = (unsigned long)trampoline;
    regs->sp = top - 256;
    regs->a0 = (unsigned long)signum;
}

static void sys_sigreturn(struct pt_regs *regs)
{
    struct task_struct *current = get_current();

    if (!current || !current->in_signal)
        return;

    trap_puts("[Signal] sigreturn\n");

    *regs = current->saved_signal_frame;
    current->in_signal = 0;

    if (current->signal_stack)
    {
        free(current->signal_stack);
        current->signal_stack = 0;
    }
}

static int sys_kill(long pid, int signum)
{
    if (signum <= 0 || signum >= MAX_SIGNAL)
        return -1;

    struct task_struct *task = find_task((int)pid);
    if (!task || task->kind != TASK_USER)
        return -1;

    if (task->signal_handlers[signum])
    {
        task->pending_signals |= (1UL << signum);
        if (task->state == TASK_SLEEPING || task->state == TASK_WAITING)
            task->state = TASK_RUNNING;
    }
    else
    {
        user_console_release_pid(task->pid);
        mark_task_zombie(task, 128 + signum);
    }

    return 0;
}

static int handle_legacy_ecall_demo(struct pt_regs *regs)
{
    struct task_struct *current = get_current();
    unsigned long sepc_offset;

    if (!current || !current->legacy_ecall_demo)
        return 0;

    sepc_offset = regs->sepc;
    if (current->user_image &&
        sepc_offset >= (unsigned long)current->user_image)
    {
        sepc_offset -= (unsigned long)current->user_image;
    }

    trap_puts("=== S-Mode trap ===\n");
    trap_puts("scause: ");
    trap_put_uint(regs->scause);
    trap_puts("\n");
    trap_puts("sepc: ");
    trap_hex32(sepc_offset);
    trap_puts("\n");
    trap_puts("stval: ");
    trap_put_uint(regs->stval);
    trap_puts("\n");

    regs->sepc += 4;
    regs->a0 = 0;

    current->legacy_ecall_count++;
    if (current->legacy_ecall_count >= 5)
        process_exit_current(0);

    return 1;
}

static void handle_user_syscall(struct pt_regs *regs)
{
    struct task_struct *current = get_current();
    long ret = -1;
    int advance = 1;

    if (current)
        current->trap_frame = regs;

    if (handle_legacy_ecall_demo(regs))
        return;

    switch (regs->a7)
    {
    case SYS_GETPID:
        ret = current ? current->pid : -1;
        break;
    case SYS_UART_READ:
        ret = sys_uart_read((char *)regs->a0, (long)regs->a1);
        break;
    case SYS_UART_WRITE:
        ret = sys_uart_write((const char *)regs->a0, (long)regs->a1);
        break;
    case SYS_EXEC:
        ret = sys_exec(regs, (const char *)regs->a0);
        if (ret == 0)
            advance = 0;
        break;
    case SYS_FORK:
        ret = fork_current_process(regs);
        break;
    case SYS_WAITPID:
        regs->sepc += 4;
        regs->a0 = wait_for_child((long)regs->a0);
        return;
    case SYS_EXIT:
    case SYS_LEGACY_EXIT:
        regs->sepc += 4;
        process_exit_current((int)regs->a0);
        return;
    case SYS_STOP:
        ret = stop_task((long)regs->a0);
        break;
    case SYS_DISPLAY:
        video_bmp_display((unsigned int *)regs->a0, (int)regs->a1,
                          (int)regs->a2);
        ret = 0;
        break;
    case SYS_USLEEP:
        regs->sepc += 4;
        process_sleep_usec((unsigned int)regs->a0);
        regs->a0 = 0;
        return;
    case SYS_SIGNAL:
        if ((int)regs->a0 > 0 && (int)regs->a0 < MAX_SIGNAL)
        {
            ret = current->signal_handlers[regs->a0];
            current->signal_handlers[regs->a0] = regs->a1;
        }
        break;
    case SYS_SIGRETURN:
        sys_sigreturn(regs);
        return;
    case SYS_KILL:
        ret = sys_kill((long)regs->a0, (int)regs->a1);
        break;
    default:
        ret = -1;
        break;
    }

    if (advance)
        regs->sepc += 4;
    regs->a0 = (unsigned long)ret;
}

/* ---------- Ring buffer for asynchronous UART RX/TX ---------- */
#define RING_SIZE 16384
struct ringbuf
{
    char buf[RING_SIZE];
    volatile unsigned int r;
    volatile unsigned int w;
};

static struct ringbuf rx_ring;
static struct ringbuf tx_ring;
static int async_console_enabled = 0;
#define SHELL_LINE_SIZE 160
static char shell_line[SHELL_LINE_SIZE];
static int shell_output_busy = 0;
static int boot_time_deferred = 0;
static unsigned long boot_time_deferred_sec = 0;

static int ring_empty(struct ringbuf *rb)
{
    return rb->r == rb->w;
}

static int ring_full(struct ringbuf *rb)
{
    return ((rb->w + 1) % RING_SIZE) == rb->r;
}

static void ring_push(struct ringbuf *rb, char c)
{
    unsigned int next = (rb->w + 1) % RING_SIZE;
    if (next == rb->r)
        return;
    rb->buf[rb->w] = c;
    rb->w = next;
}

static int ring_pop(struct ringbuf *rb, char *c)
{
    if (ring_empty(rb))
        return 0;
    *c = rb->buf[rb->r];
    rb->r = (rb->r + 1) % RING_SIZE;
    return 1;
}

static void ring_clear(struct ringbuf *rb)
{
    rb->r = 0;
    rb->w = 0;
}

static int console_tx_pending(void)
{
    return async_console_enabled && !ring_empty(&tx_ring);
}

#define UART_TX_BACKGROUND_BUDGET 8
#define UART_TX_AGGRESSIVE_BUDGET 256
#define UART_TX_READY_SPINS 1024

static int uart_kick_tx(void)
{
    int sent = 0;

    while (!ring_empty(&tx_ring) &&
           (uart_read_reg(UART_LSR) & UART_LSR_THRE))
    {
        char c;
        if (ring_pop(&tx_ring, &c))
        {
            uart_write_reg(UART_THR, (unsigned char)c);
            sent++;
        }
    }

    if (!ring_empty(&tx_ring))
        uart_enable_tx_irq();
    else
        uart_disable_tx_irq();

    return sent;
}

static void console_putc_async(char c)
{
    unsigned long flags;

    if (!async_console_enabled)
    {
        uart_putc_polling(c);
        return;
    }

    if (c == '\n')
        console_putc_async('\r');

    while (1)
    {
        flags = irq_save();

        if (!ring_full(&tx_ring))
        {
            ring_push(&tx_ring, c);
            uart_kick_tx();
            irq_restore(flags);
            return;
        }

        irq_restore(flags);
        run_tasks();
        uart_pump_tx_aggressive();
    }
}

static void console_puts_async(const char *s)
{
    while (*s)
        console_putc_async(*s++);
}

static void console_flush_async(void)
{
    if (!async_console_enabled)
        return;

    while (!ring_empty(&tx_ring) ||
           !(uart_read_reg(UART_LSR) & UART_LSR_THRE))
    {
        run_tasks();
        uart_pump_tx_aggressive();
    }
}

static void console_hex_async(unsigned long h)
{
    const char *hex = "0123456789abcdef";
    console_puts_async("0x");
    for (int i = (int)(sizeof(unsigned long) * 2) - 1; i >= 0; i--)
    {
        console_putc_async(hex[(h >> (i * 4)) & 0xf]);
    }
}

static void console_put_uint_async(unsigned long x)
{
    char buf[32];
    int i = 0;

    if (x == 0)
    {
        console_putc_async('0');
        return;
    }

    while (x > 0)
    {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0)
        console_putc_async(buf[--i]);
}

/* ---------- Task queue: advanced exercise 2 ---------- */
typedef void (*task_callback_t)(void *arg);

#define MAX_TASKS 64
struct task
{
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

void add_task(task_callback_t callback, void *arg, int priority)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (!tasks[i].used)
        {
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

static int pick_task_above(int min_priority)
{
    int best = -1;

    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (!tasks[i].used)
            continue;
        if (tasks[i].priority <= min_priority)
            continue;
        if (best < 0 ||
            tasks[i].priority > tasks[best].priority ||
            (tasks[i].priority == tasks[best].priority && tasks[i].seq < tasks[best].seq))
        {
            best = i;
        }
    }

    return best;
}

static void run_tasks(void)
{
    unsigned long flags = irq_save();

    int previous_running = tasks_running;
    int previous_priority = current_task_priority;

    tasks_running = 1;

    while (1)
    {
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

static char console_getc(void)
{
    char c;

    while (!ring_pop(&rx_ring, &c))
    {
        run_tasks();
        uart_pump_tx();
        schedule();
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
char uart_getc(void)
{
    if (!async_console_enabled)
        return uart_getc_polling();

    return console_getc();
}

void uart_putc(char c)
{
    console_putc_async(c);
}

void uart_puts(const char *s)
{
    console_puts_async(s);
}

/* ---------- PLIC / UART interrupt ---------- */

static void plic_init(void)
{
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base)
    {
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

static int plic_claim(void)
{
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base)
        return 0;

    return (int)read32(plic_claim_addr(ctx));
}

static void plic_complete(int irq)
{
    int ctx = (int)PLIC_CONTEXT(boot_cpu_hartid);

    if (!plic_base)
        return;

    write32(plic_claim_addr(ctx), (unsigned int)irq);
}

#define UART_LCR_8N1 0x03

static void uart_interrupt_init(void)
{
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);

    unsigned int mcr = uart_read_reg(UART_MCR);
    uart_write_reg(UART_MCR, mcr | UART_MCR_OUT2);
}

static void uart_interrupt_disable(void)
{
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow &= ~(UART_IER_RX | UART_IER_TX);
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void uart_interrupt_enable(void)
{
    uart_ier_shadow = uart_read_reg(UART_IER);
    uart_ier_shadow |= UART_IER_RX;
    uart_write_reg(UART_IER, uart_ier_shadow);
}

static void uart_finish_task_irq_state(void)
{
    unsigned long flags = irq_save();

    uart_kick_tx();

    irq_restore(flags);
}

static void uart_wait_tx_ready(unsigned int spins)
{
    while (spins-- > 0)
    {
        if (uart_read_reg(UART_LSR) & UART_LSR_THRE)
            return;
    }
}

static void uart_pump_tx_budget(unsigned int budget, unsigned int wait_spins)
{
    if (!async_console_enabled)
        return;

    while (budget-- > 0)
    {
        unsigned long flags = irq_save();
        int sent = uart_kick_tx();
        int empty = ring_empty(&tx_ring);
        irq_restore(flags);

        if (empty)
            return;

        if (sent == 0)
        {
            if (wait_spins == 0)
                return;

            uart_wait_tx_ready(wait_spins);
        }
    }
}

static void uart_pump_tx(void)
{
    uart_pump_tx_budget(UART_TX_BACKGROUND_BUDGET, 0);
}

static void uart_pump_tx_aggressive(void)
{
    uart_pump_tx_budget(UART_TX_AGGRESSIVE_BUDGET, UART_TX_READY_SPINS);
}

#define UART_IRQ_TASK_COUNT 4
#define UART_IRQ_RX_MAX 64

struct uart_irq_task
{
    int used;
    int rx_len;
    int tx_ready;
    char rx[UART_IRQ_RX_MAX];
};

static struct uart_irq_task uart_irq_tasks[UART_IRQ_TASK_COUNT];

static void uart_processing_task(void *arg)
{
    struct uart_irq_task *task = (struct uart_irq_task *)arg;

    for (int i = 0; i < task->rx_len; i++)
        ring_push(&rx_ring, task->rx[i] == '\r' ? '\n' : task->rx[i]);

    task->rx_len = 0;
    task->tx_ready = 0;
    task->used = 0;

    uart_finish_task_irq_state();
}

static struct uart_irq_task *alloc_uart_irq_task(void)
{
    for (int i = 0; i < UART_IRQ_TASK_COUNT; i++)
    {
        if (!uart_irq_tasks[i].used)
        {
            uart_irq_tasks[i].used = 1;
            uart_irq_tasks[i].rx_len = 0;
            uart_irq_tasks[i].tx_ready = 0;
            return &uart_irq_tasks[i];
        }
    }

    return 0;
}

static void handle_uart_interrupt(void)
{
    (void)uart_read_reg(UART_IIR);
    uart_interrupt_disable();

    struct uart_irq_task *task = alloc_uart_irq_task();
    if (!task)
    {
        while (uart_read_reg(UART_LSR) & UART_LSR_DR)
            (void)uart_read_reg(UART_RBR);
        uart_finish_task_irq_state();
        return;
    }

    while ((uart_read_reg(UART_LSR) & UART_LSR_DR) &&
           task->rx_len < UART_IRQ_RX_MAX)
    {
        task->rx[task->rx_len++] =
            (char)(uart_read_reg(UART_RBR) & 0xff);
    }

    if (uart_read_reg(UART_LSR) & UART_LSR_THRE)
        task->tx_ready = 1;

    add_task(uart_processing_task, task, 3);
}

/* ---------- Timer multiplexing: advanced exercise 1 ---------- */
typedef void (*timer_callback_t)(void *arg);

static int timer_boot_log_enabled = 0;

static void boot_time_print_line(unsigned long sec)
{
    console_puts_async("boot time: ");
    console_put_uint_async(sec);
    console_puts_async("\n");
}

static void boot_time_task(void *arg)
{
    unsigned long sec = (unsigned long)arg;

    if (shell_output_busy)
    {
        boot_time_deferred = 1;
        boot_time_deferred_sec = sec;
        return;
    }

    boot_time_print_line(sec);
    console_flush_async();
}

static void boot_time_print_deferred(void)
{
    if (!boot_time_deferred || shell_output_busy)
        return;

    unsigned long sec = boot_time_deferred_sec;

    boot_time_deferred = 0;
    boot_time_task((void *)sec);
}

#define MAX_TIMERS 64
struct timer_event
{
    int used;
    unsigned long expire;
    timer_callback_t cb;
    void *arg;
};

static struct timer_event timers[MAX_TIMERS];
static unsigned long next_periodic_tick = 0;
static unsigned long next_scheduler_tick = 0;
static unsigned long boot_time_base = 0;

static unsigned long ticks_per_sec(void)
{
    return timebase_frequency ? timebase_frequency : 10000000UL;
}

static unsigned long now_seconds(void)
{
    return (read_time() - boot_time_base) / ticks_per_sec();
}

static void program_next_timer(void)
{
    unsigned long next = next_periodic_tick;

    if (next_scheduler_tick && (next == 0 || next_scheduler_tick < next))
        next = next_scheduler_tick;

    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (!timers[i].used)
            continue;
        if (next == 0 || timers[i].expire < next)
            next = timers[i].expire;
    }

    if (next == 0)
        next = read_time() + 2 * ticks_per_sec();

    sbi_set_timer(next);
}

void add_timer(timer_callback_t callback, void *arg, int sec)
{
    if (sec < 0)
        sec = 0;

    unsigned long expire = read_time() + (unsigned long)sec * ticks_per_sec();

    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (!timers[i].used)
        {
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

struct timeout_message
{
    int used;
    unsigned long created_sec;
    int seconds;
    char msg[96];
};

static struct timeout_message timeout_messages[MAX_TIMERS];

static void timeout_task(void *arg)
{
    struct timeout_message *tm = (struct timeout_message *)arg;

    console_puts_async(tm->msg);
    console_puts_async("\n");

    tm->used = 0;
}

static void timeout_timer_cb(void *arg)
{
    add_task(timeout_task, arg, 2);
}

static void handle_timer_interrupt(void)
{
    unsigned long now = read_time();
    unsigned long sched_delta = ticks_per_sec() / 32;

    uart_pump_tx();

    if (sched_delta == 0)
        sched_delta = 1;

    while (next_scheduler_tick && now >= next_scheduler_tick)
        next_scheduler_tick += sched_delta;

    while (next_periodic_tick && now >= next_periodic_tick)
    {
        if (timer_boot_log_enabled)
        {
            unsigned long sec =
                (next_periodic_tick - boot_time_base) / ticks_per_sec();

            add_task(boot_time_task, (void *)sec, 2);
        }

        next_periodic_tick += 2 * ticks_per_sec();
    }

    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].used && now >= timers[i].expire)
        {
            timer_callback_t cb = timers[i].cb;
            void *arg = timers[i].arg;
            timers[i].used = 0;
            add_task((task_callback_t)cb, arg, 2);
        }
    }

    program_next_timer();
}

static void timer_init(void)
{
    boot_time_base = read_time();
    next_periodic_tick = boot_time_base + 2 * ticks_per_sec();
    next_scheduler_tick = boot_time_base + ticks_per_sec() / 32;

    /*
     * Do not enable STIE here.
     * start_kernel() will enable it after printing the prompt.
     */
    program_next_timer();
}

/* ---------- trap handler ---------- */
void do_trap(struct pt_regs *regs)
{
    unsigned long scause = regs->scause;
    struct task_struct *current = get_current();

    if (current && current->kind == TASK_USER)
        current->trap_frame = regs;

    if (scause == SCAUSE_U_ECALL)
    {
        handle_user_syscall(regs);
    }
    else if (scause == SCAUSE_S_TIMER)
    {
        handle_timer_interrupt();
        process_wake_sleepers();
    }
    else if (scause == SCAUSE_S_EXT)
    {
        int irq = plic_claim();

        if (irq == uart_irq_id)
        {
            handle_uart_interrupt();
        }

        if (irq)
            plic_complete(irq);
    }
    else
    {
        trap_puts("Unexpected trap. sepc: ");
        trap_hex(regs->sepc);
        trap_puts(", scause: ");
        trap_hex(regs->scause);
        trap_puts(", stval: ");
        trap_hex(regs->stval);
        trap_puts("\n");
        while (1)
        {
        }
    }

    run_tasks();

    if (current && scheduler_ready &&
        (scause == SCAUSE_S_TIMER || current->state != TASK_RUNNING))
    {
        schedule();
    }

    deliver_signal_if_needed(regs);
}

/* ---------- load command ---------- */
static unsigned int uart_get_u32_polling(void)
{
    unsigned int x = 0;
    x |= (unsigned int)uart_getb();
    x |= (unsigned int)uart_getb() << 8;
    x |= (unsigned int)uart_getb() << 16;
    x |= (unsigned int)uart_getb() << 24;
    return x;
}

static void boot_loaded_kernel(const void *fdt)
{
    void (*kernel_entry)(unsigned long hartid, const void *fdt);
    kernel_entry = (void (*)(unsigned long, const void *))LOAD_ADDR;
    asm volatile("fence.i" ::: "memory");
    kernel_entry(0, fdt);
}

static void shell_load(void)
{
    int old_timer_log = timer_boot_log_enabled;

    timer_boot_log_enabled = 0;

    uart_interrupt_disable();

    ring_clear(&rx_ring);
    ring_clear(&tx_ring);

    async_console_enabled = 0;

    uart_puts("Waiting for kernel image...\n");

    unsigned int magic = uart_get_u32_polling();

    if (magic != BOOT_MAGIC)
    {
        uart_puts("Bad magic.\n");

        async_console_enabled = 1;
        uart_interrupt_enable();
        timer_boot_log_enabled = old_timer_log;
        return;
    }

    unsigned int size = uart_get_u32_polling();

    uart_puts("Receiving kernel...\n");

    unsigned char *dst = (unsigned char *)LOAD_ADDR;
    for (unsigned int i = 0; i < size; i++)
    {
        dst[i] = uart_getb();
    }

    uart_puts("Waiting for cpio archive...\n");

    magic = uart_get_u32_polling();
    if (magic != BOOT_MAGIC)
    {
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
    if (!new_fdt)
    {
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
    asm volatile("csrc sie, %0" ::"r"(SIE_STIE | SIE_SEIE) : "memory");

    boot_loaded_kernel(new_fdt);
}

/* ---------- shell ---------- */
static int parse_uint(const char **p)
{
    int v = 0;
    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9')
    {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

static void test_task_cb(void *arg)
{
    char *s = (char *)arg;
    console_puts_async("[Task] Executing Priority ");
    console_puts_async(s);
    console_puts_async("\n");
}

static void shell_help(void)
{
    console_puts_async("Available commands:\n");
    console_puts_async("    help        - show all commands.\n");
    console_puts_async("    hello       - print Hello world.\n");
    console_puts_async("    info        - print system info.\n");
    console_puts_async("    load        - load a kernel and cpio over UART.\n");
    console_puts_async("    ls          - list files.\n");
    console_puts_async("    cat         - show file content.\n");
    console_puts_async("    memtest     - run memory allocator test.\n");
    console_puts_async("    exec FILE   - execute a user program.\n");
    console_puts_async("    settimeout  - show text after X sec.\n");
    console_puts_async("    tasktest    - test priority task queue.\n");
}

static void shell_info(void)
{
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

static void shell_set_timeout(const char *cmd)
{
    const char *p = cmd;
    while (*p && *p != ' ')
        p++;

    int sec = parse_uint(&p);

    while (*p == ' ')
        p++;

    if (*p == '\0')
    {
        console_puts_async("Usage: settimeout SECONDS MESSAGE\n");
        return;
    }

    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (!timeout_messages[i].used)
        {
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

static void shell_execute(const char *cmd)
{
    if (strcmp_simple(cmd, "help"))
    {
        shell_help();
    }
    else if (strcmp_simple(cmd, "hello"))
    {
        console_puts_async("Hello world.\n");
    }
    else if (strcmp_simple(cmd, "info"))
    {
        shell_info();
    }
    else if (strcmp_simple(cmd, "load"))
    {
        shell_load();
    }
    else if (strcmp_simple(cmd, "ls"))
    {
        if (initrd_start)
            initrd_list(initrd_start);
        else
            console_puts_async("initrd not found\n");
    }
    else if (strncmp_simple(cmd, "cat ", 4) == 0)
    {
        if (initrd_start)
            initrd_cat(initrd_start, cmd + 4);
        else
            console_puts_async("initrd not found\n");
    }
    else if (strcmp_simple(cmd, "memtest"))
    {
        test_alloc_1();
    }
    else if (strcmp_simple(cmd, "exec"))
    {
        console_puts_async("Usage: exec FILE\n");
    }
    else if (strncmp_simple(cmd, "exec ", 5) == 0)
    {
        const char *filename = cmd + 5;

        while (*filename == ' ')
            filename++;

        if (*filename == '\0')
            console_puts_async("Usage: exec FILE\n");
        else
            exec_user_program(filename);
    }
    else if (strncmp_simple(cmd, "setTimeout ", 11) == 0 ||
             strncmp_simple(cmd, "settimeout ", 11) == 0)
    {
        shell_set_timeout(cmd);
    }
    else if (strcmp_simple(cmd, "tasktest"))
    {
        add_task(test_task_cb, "1", 1);
        add_task(test_task_cb, "3", 3);
        add_task(test_task_cb, "2", 2);
        run_tasks();
    }
    else if (cmd[0] != '\0')
    {
        console_puts_async("Unknown command: ");
        console_puts_async(cmd);
        console_puts_async("\nUse help to get commands.\n");
    }
}

/* ---------- self relocation ---------- */
static void relocate_self(const void *fdt)
{
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

    asm volatile("mv sp, %0" ::"r"(new_sp) : "memory");

    void (*entry)(const void *) =
        (void (*)(const void *))(RELOC_ADDR + ((unsigned long)start_kernel - (unsigned long)_start));

    asm volatile("fence.i" ::: "memory");
    entry(fdt);
}

static void shell_print_prompt(int leading_newline)
{
    if (leading_newline)
        console_putc_async('\n');

    console_puts_async("opi-rv2> ");
    console_flush_async();
}

static void shell_thread(void *arg)
{
    (void)arg;

    int idx = 0;

    console_puts_async("\nType help to get commands.\n\n");
    shell_print_prompt(0);
    add_task(boot_time_task, (void *)0, 2);
    timer_boot_log_enabled = 1;

    while (1)
    {
        char c = console_getc();

        if (c == '\r' || c == '\n')
        {
            console_putc_async('\n');
            console_flush_async();
            shell_line[idx] = '\0';

            shell_output_busy = 1;
            shell_execute(shell_line);
            console_flush_async();
            shell_output_busy = 0;

            idx = 0;
            shell_print_prompt(1);
            boot_time_print_deferred();
        }
        else if (c == 127 || c == '\b')
        {
            if (idx > 0)
            {
                idx--;
                console_puts_async("\b \b");
                console_flush_async();
                if (idx == 0)
                    boot_time_print_deferred();
            }
        }
        else
        {
            if (idx < SHELL_LINE_SIZE - 1)
            {
                shell_line[idx++] = c;
                console_putc_async(c);
                console_flush_async();
            }
        }
    }
}

/* ---------- kernel entry ---------- */
void start_kernel(const void *fdt)
{
    /*
     * Important for hot-loaded kernel:
     * Previous kernel may jump here with SIE/STIE/SEIE still enabled.
     */
    local_irq_disable();
    asm volatile("csrc sie, %0" ::"r"(SIE_STIE | SIE_SEIE) : "memory");

    boot_fdt = fdt;
    uart_init_from_dtb(fdt);

    if ((unsigned long)_start < RELOC_ADDR)
    {
        uart_puts("\nRelocating kernel...\n");
        relocate_self(fdt);
        while (1)
        {
        }
    }

    set_stvec_relocated();

    uart_puts("\nStarting kernel ...\n");

    uart_puts("fdt ptr = ");
    uart_hex((unsigned long)fdt);
    uart_puts("\n");

    initrd_init_from_dtb(fdt);
    mm_init_advanced(fdt, (unsigned long)initrd_start, (unsigned long)initrd_end);
    uart_puts("memory allocator ready\n");
    timer_frequency_init_from_dtb(fdt);
    plic_init_from_dtb(fdt);
    video_init();

    plic_init();
    uart_interrupt_init();
    timer_init();

    enable_timer_interrupt();
    enable_external_interrupt();
    local_irq_enable();

    async_console_enabled = 1;

    memset(&boot_task, 0, sizeof(boot_task));
    boot_task.pid = 0;
    boot_task.state = TASK_RUNNING;
    boot_task.kind = TASK_KERNEL;
    boot_task.waiting_pid = -1;
    current_task = &boot_task;
    enqueue_task(&boot_task);
    asm volatile("mv tp, %0" ::"r"(&boot_task) : "memory");
    scheduler_ready = 1;

    create_kernel_thread(shell_thread, 0);
    idle_thread(0);
}
