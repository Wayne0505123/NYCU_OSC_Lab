#include "mm.h"
#include <stdint.h>
#include <stddef.h>

/* ---------- UART ---------- */
extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern unsigned char uart_getb_raw(void);
extern void uart_set_base(unsigned long base);
extern void uart_set_config(int reg_shift, int reg_io_width);

/* ---------- Linker symbols ---------- */
extern char _start[];
extern char _end[];
void start_kernel(const void *fdt);

/* ---------- Boot / load / relocation ---------- */
#define LOAD_ADDR          ((unsigned char *)0x00200000UL)
#define CPIO_LOAD_ADDR     ((unsigned char *)0x03000000UL)
#define NEW_FDT_ADDR       ((unsigned char *)0x03F00000UL)
#define NEW_FDT_SIZE       0x400000UL
#define RELOC_ADDR         0x20000000UL
#define BOOT_MAGIC         0x544F4F42UL
#define KERNEL_STACK_SIZE  (128 * 1024UL)

/* ---------- Orange Pi RV2 UART0 ---------- */
#define UART_BASE 0xD4017000UL

static const void *boot_fdt;
static const void *initrd_start = 0;
static const void *initrd_end = 0;

/* ---------- SBI base extension: kept for Lab 1/2 info command only ---------- */
#define SBI_EXT_BASE 0x10

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

static unsigned long sbi_get_spec_version(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 0, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_id(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 1, 0, 0, 0, 0, 0, 0).value;
}

static unsigned long sbi_get_impl_version(void) {
    return (unsigned long)sbi_ecall(SBI_EXT_BASE, 2, 0, 0, 0, 0, 0, 0).value;
}

/* Defensive cleanup when this kernel is hot-loaded from a Lab4 kernel. */
static inline void local_irq_disable(void) {
    asm volatile("csrci sstatus, 2" ::: "memory");
    asm volatile("csrc sie, %0" :: "r"((1UL << 5) | (1UL << 9)) : "memory");
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

static void uart_put_ulong(unsigned long x) {
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

/* ---------- FDT parser: Lab 2 + used by Lab 3 allocator ---------- */
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

int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (!fdt_is_valid(fdt))
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

    if (!fdt_is_valid(fdt))
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

static void initrd_init_from_dtb(const void *fdt) {
    int offset = fdt_path_offset(fdt, "/chosen");
    int len;

    if (offset < 0)
        return;

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

    if (node >= 0) {
        const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
        if (reg && len >= 16)
            uart_base = read_cells(reg, 2);

        int shift_len = 0;
        int width_len = 0;
        const uint32_t *shift = (const uint32_t *)fdt_getprop(fdt, node, "reg-shift", &shift_len);
        const uint32_t *width = (const uint32_t *)fdt_getprop(fdt, node, "reg-io-width", &width_len);
        reg_shift = (shift && shift_len >= 4) ? (int)bswap32_main(shift[0]) : 2;
        reg_width = (width && width_len >= 4) ? (int)bswap32_main(width[0]) : 4;
    }

    uart_set_base(uart_base);
    uart_set_config(reg_shift, reg_width);

    uart_puts("uart base from dtb = ");
    uart_hex(uart_base);
    uart_puts("\n");
    uart_puts("uart reg shift = ");
    uart_hex((unsigned long)reg_shift);
    uart_puts("\n");
    uart_puts("uart reg width = ");
    uart_hex((unsigned long)reg_width);
    uart_puts("\n");
}

/* ---------- initramfs / cpio ---------- */
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

static void initrd_list(const void *rd) {
    const char *p = (const char *)rd;

    while (p && p < (const char *)initrd_end) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (memcmp_simple(hdr->magic, "070701", 6) != 0) {
            uart_puts("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            return;

        uart_put_ulong((unsigned long)filesize);
        uart_putc(' ');
        uart_puts(name);
        uart_putc('\n');

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
        uart_puts("initrd_cat: ");
        uart_puts(filename);
        uart_puts(": No such file\n");
        return;
    }

    for (int i = 0; i < size; i++)
        uart_putc(data[i]);
    uart_putc('\n');
}

/* ---------- load command ---------- */
static unsigned int uart_get_u32_raw(void) {
    unsigned int x = 0;
    x |= (unsigned int)uart_getb_raw();
    x |= (unsigned int)uart_getb_raw() << 8;
    x |= (unsigned int)uart_getb_raw() << 16;
    x |= (unsigned int)uart_getb_raw() << 24;
    return x;
}

static void boot_loaded_kernel(const void *fdt) {
    void (*kernel_entry)(unsigned long hartid, const void *fdt);
    kernel_entry = (void (*)(unsigned long, const void *))LOAD_ADDR;
    asm volatile("fence.i" ::: "memory");
    kernel_entry(0, fdt);
}

static void shell_load(void) {
    uart_puts("Waiting for kernel image...\n");

    unsigned int magic = uart_get_u32_raw();
    if (magic != BOOT_MAGIC) {
        uart_puts("Bad magic.\n");
        return;
    }

    unsigned int size = uart_get_u32_raw();
    uart_puts("Receiving kernel...\n");

    unsigned char *dst = (unsigned char *)LOAD_ADDR;
    for (unsigned int i = 0; i < size; i++)
        dst[i] = uart_getb_raw();

    uart_puts("Waiting for cpio archive...\n");

    magic = uart_get_u32_raw();
    if (magic != BOOT_MAGIC) {
        uart_puts("Bad cpio magic.\n");
        return;
    }

    unsigned int cpio_size = uart_get_u32_raw();
    uart_puts("Receiving cpio...\n");

    unsigned char *cpio_dst = (unsigned char *)CPIO_LOAD_ADDR;
    for (unsigned int i = 0; i < cpio_size; i++)
        cpio_dst[i] = uart_getb_raw();

    void *new_fdt = make_writable_fdt_copy(boot_fdt);
    if (!new_fdt)
        return;

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
    boot_loaded_kernel(new_fdt);
}

/* ---------- shell ---------- */
static void print_prompt(void) {
    uart_puts("opi-rv2> ");
}

static void shell_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("    help     - show all commands.\n");
    uart_puts("    hello    - print Hello world.\n");
    uart_puts("    info     - print system info.\n");
    uart_puts("    load     - load a kernel and cpio over UART.\n");
    uart_puts("    ls       - list files in initramfs.\n");
    uart_puts("    cat      - show file content.\n");
    uart_puts("    memtest  - run memory allocator test.\n");
}

static void shell_info(void) {
    uart_puts("System information:\n");
    uart_puts("    OpenSBI specification version: ");
    uart_hex(sbi_get_spec_version());
    uart_puts("\n");

    uart_puts("    implementation ID: ");
    uart_hex(sbi_get_impl_id());
    uart_puts("\n");

    uart_puts("    implementation version: ");
    uart_hex(sbi_get_impl_version());
    uart_puts("\n");
}

static void shell_execute(const char *cmd) {
    if (strcmp_simple(cmd, "help")) {
        shell_help();
    } else if (strcmp_simple(cmd, "hello")) {
        uart_puts("Hello world.\n");
    } else if (strcmp_simple(cmd, "info")) {
        shell_info();
    } else if (strcmp_simple(cmd, "load")) {
        shell_load();
    } else if (strcmp_simple(cmd, "ls")) {
        if (initrd_start)
            initrd_list(initrd_start);
        else
            uart_puts("initrd not found\n");
    } else if (strncmp_simple(cmd, "cat ", 4) == 0) {
        if (initrd_start)
            initrd_cat(initrd_start, cmd + 4);
        else
            uart_puts("initrd not found\n");
    } else if (strcmp_simple(cmd, "memtest")) {
        test_alloc_1();
    } else if (cmd[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\nUse help to get commands.\n");
    }
}

/* ---------- self relocation ---------- */
static void relocate_self(const void *fdt) {
    unsigned char *src = (unsigned char *)_start;
    unsigned char *dst = (unsigned char *)RELOC_ADDR;
    unsigned long size = (unsigned long)(_end - _start);

    for (unsigned long i = 0; i < size; i++)
        dst[i] = src[i];

    unsigned long kernel_size = (unsigned long)_end - (unsigned long)_start;
    unsigned long new_sp = RELOC_ADDR + kernel_size + KERNEL_STACK_SIZE;
    new_sp &= ~0xFUL;

    asm volatile("mv sp, %0" :: "r"(new_sp) : "memory");

    void (*entry)(const void *) =
        (void (*)(const void *))(RELOC_ADDR + ((unsigned long)start_kernel - (unsigned long)_start));

    asm volatile("fence.i" ::: "memory");
    entry(fdt);
}

/* ---------- kernel entry ---------- */
void start_kernel(const void *fdt) {
    local_irq_disable();
    boot_fdt = fdt;
    uart_set_base(UART_BASE);

    if ((unsigned long)_start < RELOC_ADDR) {
        uart_puts("\nRelocating kernel...\n");
        relocate_self(fdt);
        while (1) {}
    }

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

    uart_puts("\nType help to get commands.\n\n");
    print_prompt();

    while (1) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            buf[idx] = '\0';
            shell_execute(buf);
            idx = 0;
            uart_putc('\n');
            print_prompt();
        } else if (c == 127 || c == '\b') {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
        } else {
            if (idx < (int)sizeof(buf) - 1) {
                buf[idx++] = c;
                uart_putc(c);
            }
        }
    }
}
