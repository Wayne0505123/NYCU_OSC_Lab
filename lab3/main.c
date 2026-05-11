#include "mm.h"
#include <stdint.h>
#include <stddef.h>

/* ---------- UART ---------- */

extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_put_uint(unsigned int x);
extern unsigned char uart_getb(void);
extern void uart_putb(unsigned char c);
extern void uart_set_base(unsigned long base);
extern void uart_set_config(int reg_shift, int reg_io_width);

/* ---------- Linker symbols ---------- */

extern char _start[];
extern char _end[];

/* ---------- Load / relocation ---------- */

#define LOAD_ADDR  ((unsigned char *)0x00200000UL)
#define RELOC_ADDR 0x20000000UL
#define BOOT_MAGIC 0x544F4F42UL

static const void *boot_fdt;

/* start_kernel is used by relocate_self() */
void start_kernel(const void *fdt);

/* ---------- SBI ---------- */

#define SBI_EXT_SET_TIMER 0x0
#define SBI_EXT_SHUTDOWN  0x8
#define SBI_EXT_BASE      0x10

enum sbi_ext_base_fid {
    SBI_EXT_BASE_GET_SPEC_VERSION,
    SBI_EXT_BASE_GET_IMP_ID,
    SBI_EXT_BASE_GET_IMP_VERSION,
    SBI_EXT_BASE_PROBE_EXT,
    SBI_EXT_BASE_GET_MVENDORID,
    SBI_EXT_BASE_GET_MARCHID,
    SBI_EXT_BASE_GET_MIMPID,
};

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(int ext,
                        int fid,
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

    ret.error = a0;
    ret.value = a1;
    return ret;
}

static long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_SPEC_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

static long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_ID,
                     0, 0, 0, 0, 0, 0).value;
}

static long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

/* ---------- FDT ---------- */

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

static unsigned long initrd_start = 0;
static unsigned long initrd_end = 0;

/* ---------- Simple helpers ---------- */

static uint32_t bswap32_main(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
}

/* Needed because we build with -ffreestanding -nostdlib */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;

    while (n--)
        *p++ = (unsigned char)c;

    return s;
}

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

static int strcmp_cmd(const char *a, const char *b) {
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

static const void *align_ptr(const void *ptr, size_t align) {
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static unsigned long read_cells(const uint32_t *p, int cells) {
    unsigned long v = 0;

    for (int i = 0; i < cells; i++)
        v = (v << 32) | bswap32_main(p[i]);

    return v;
}

/* ---------- FDT parser, used by mm.c too ---------- */

int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;

    if (bswap32_main(hdr->magic) != 0xd00dfeed)
        return -1;

    const char *struct_base =
        (const char *)fdt + bswap32_main(hdr->off_dt_struct);

    const char *p = struct_base;

    char curpath[1024];
    curpath[0] = '\0';

    size_t pathlen_stack[128];
    int depth = 0;

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

            /*
             * Allow path "/memory" to match "/memory@0" or "/memory@80000000".
             */
            const char *a = curpath;
            const char *b = path;
            int matched = 1;

            while (*a || *b) {
                if (*a == '/' && *b == '/') {
                    a++;
                    b++;
                    continue;
                }

                while (*a && *b &&
                       *a != '/' && *b != '/' &&
                       *a == *b) {
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

            p = (const char *)align_ptr(p + strlen_simple(name) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth > 0) {
                size_t oldlen = pathlen_stack[--depth];
                curpath[oldlen] = '\0';
            }
        } else if (tag == FDT_PROP) {
            uint32_t len = bswap32_main(*(const uint32_t *)p);
            p += 4; /* len */
            p += 4; /* nameoff */
            p = (const char *)align_ptr(p + len, 4);
        } else if (tag == FDT_NOP) {
            /* nothing */
        } else if (tag == FDT_END) {
            break;
        } else {
            return -1;
        }
    }

    return -1;
}

const void *fdt_getprop(const void *fdt,
                        int nodeoffset,
                        const char *name,
                        int *lenp) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;

    if (bswap32_main(hdr->magic) != 0xd00dfeed)
        return 0;

    const char *struct_base =
        (const char *)fdt + bswap32_main(hdr->off_dt_struct);

    const char *strings_base =
        (const char *)fdt + bswap32_main(hdr->off_dt_strings);

    const char *p = struct_base + nodeoffset;

    if (bswap32_main(*(const uint32_t *)p) != FDT_BEGIN_NODE)
        return 0;

    p += 4;
    p = (const char *)align_ptr(p + strlen_simple(p) + 1, 4);

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

            p = (const char *)align_ptr(p + len, 4);
        } else if (tag == FDT_BEGIN_NODE) {
            depth++;
            p = (const char *)align_ptr(p + strlen_simple(p) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth == 0)
                break;

            depth--;
        } else if (tag == FDT_NOP) {
            /* nothing */
        } else if (tag == FDT_END) {
            break;
        } else {
            return 0;
        }
    }

    return 0;
}

/* ---------- UART / initrd init ---------- */

static void uart_init_from_dtb(const void *fdt) {
    int len;

    int node = fdt_path_offset(fdt, "/soc/serial@d4017000");

    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/serial");

    if (node < 0)
        node = fdt_path_offset(fdt, "/soc/uart");

    if (node < 0) {
        uart_set_base(0xd4017000UL);
        uart_set_config(2, 4);
        return;
    }

    const uint32_t *reg =
        (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);

    if (!reg || len < 16) {
        uart_set_base(0xd4017000UL);
        uart_set_config(2, 4);
        return;
    }

    unsigned long base =
        ((unsigned long)bswap32_main(reg[0]) << 32) |
        (unsigned long)bswap32_main(reg[1]);

    const uint32_t *shift =
        (const uint32_t *)fdt_getprop(fdt, node, "reg-shift", &len);

    int reg_shift = 0;
    if (shift && len >= 4)
        reg_shift = (int)bswap32_main(shift[0]);

    const uint32_t *width =
        (const uint32_t *)fdt_getprop(fdt, node, "reg-io-width", &len);

    int reg_width = 1;
    if (width && len >= 4)
        reg_width = (int)bswap32_main(width[0]);

    uart_set_config(reg_shift, reg_width);
    uart_set_base(base);

    uart_puts("uart base from dtb = ");
    uart_hex(base);
    uart_puts("\n");

    uart_puts("uart reg shift = ");
    uart_hex((unsigned long)reg_shift);
    uart_puts("\n");

    uart_puts("uart reg width = ");
    uart_hex((unsigned long)reg_width);
    uart_puts("\n");
}

static void initrd_init_from_dtb(const void *fdt) {
    int len;

    int chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0) {
        uart_puts("initrd not found\n");
        return;
    }

    const uint32_t *prop;

    prop = (const uint32_t *)fdt_getprop(fdt, chosen,
                                         "linux,initrd-start", &len);

    if (prop && len == 8)
        initrd_start = read_cells(prop, 2);
    else if (prop && len == 4)
        initrd_start = read_cells(prop, 1);

    prop = (const uint32_t *)fdt_getprop(fdt, chosen,
                                         "linux,initrd-end", &len);

    if (prop && len == 8)
        initrd_end = read_cells(prop, 2);
    else if (prop && len == 4)
        initrd_end = read_cells(prop, 1);

    if (!initrd_start || !initrd_end) {
        uart_puts("initrd not found\n");
        return;
    }

    uart_puts("initrd start = ");
    uart_hex(initrd_start);
    uart_puts("\n");

    uart_puts("initrd end   = ");
    uart_hex(initrd_end);
    uart_puts("\n");
}

/* ---------- initrd commands ---------- */

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

static int hextoi_simple(const char *s, int n) {
    int r = 0;

    while (n-- > 0) {
        r <<= 4;

        if (*s >= '0' && *s <= '9')
            r += *s - '0';
        else if (*s >= 'A' && *s <= 'F')
            r += *s - 'A' + 10;

        s++;
    }

    return r;
}

static int align_int(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}

static void initrd_list(const void *rd) {
    const char *p = (const char *)rd;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (strncmp_simple(hdr->magic, "070701", 6) != 0) {
            uart_puts("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);

        const char *name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            return;

        uart_put_uint((unsigned int)filesize);
        uart_putc(' ');
        uart_puts(name);
        uart_putc('\n');

        const char *data = p + align_int(sizeof(struct cpio_t) + namesize, 4);
        p = data + align_int(filesize, 4);
    }
}

static void initrd_cat(const void *rd, const char *filename) {
    const char *p = (const char *)rd;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (strncmp_simple(hdr->magic, "070701", 6) != 0) {
            uart_puts("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);

        const char *name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            break;

        const char *data = p + align_int(sizeof(struct cpio_t) + namesize, 4);

        if (strcmp_full(name, filename) == 0) {
            for (int i = 0; i < filesize; i++)
                uart_putc(data[i]);

            uart_putc('\n');
            return;
        }

        p = data + align_int(filesize, 4);
    }

    uart_puts("initrd_cat: ");
    uart_puts(filename);
    uart_puts(": No such file\n");
}

/* ---------- Self relocation ---------- */

static void relocate_self(const void *fdt) {
    unsigned char *src = (unsigned char *)_start;
    unsigned char *dst = (unsigned char *)RELOC_ADDR;
    unsigned long size = (unsigned long)(_end - _start);

    for (unsigned long i = 0; i < size; i++)
        dst[i] = src[i];

    unsigned long new_sp =
        RELOC_ADDR + ((unsigned long)_end - (unsigned long)_start);

    asm volatile("mv sp, %0" :: "r"(new_sp));

    void (*entry)(const void *) =
        (void (*)(const void *))
        (RELOC_ADDR +
         ((unsigned long)start_kernel - (unsigned long)_start));

    asm volatile("fence.i" ::: "memory");

    entry(fdt);
}

/* ---------- load command ---------- */

static unsigned int uart_get_u32(void) {
    unsigned int x = 0;

    x |= (unsigned int)uart_getb();
    x |= (unsigned int)uart_getb() << 8;
    x |= (unsigned int)uart_getb() << 16;
    x |= (unsigned int)uart_getb() << 24;

    return x;
}

static void boot_loaded_kernel(void) {
    void (*kernel_entry)(unsigned long hartid, const void *fdt);

    kernel_entry =
        (void (*)(unsigned long, const void *))LOAD_ADDR;

    asm volatile("fence.i" ::: "memory");

    kernel_entry(0, boot_fdt);
}

static void shell_load(void) {
    uart_puts("Waiting for kernel image...\n");

    unsigned int magic = uart_get_u32();

    if (magic != BOOT_MAGIC) {
        uart_puts("Bad magic.\n");
        return;
    }

    unsigned int size = uart_get_u32();

    uart_puts("Receiving kernel...\n");

    for (unsigned int i = 0; i < size; i++)
        LOAD_ADDR[i] = uart_getb();

    asm volatile("fence.i" ::: "memory");

    uart_puts("Booting loaded kernel...\n");

    boot_loaded_kernel();
}

/* ---------- Shell ---------- */

static void print_prompt(void) {
    uart_puts("opi-rv2> ");
}

static void shell_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("    help    - show all commands.\n");
    uart_puts("    hello   - print Hello world.\n");
    uart_puts("    info    - print system info.\n");
    uart_puts("    load    - load next kernel over UART.\n");
    uart_puts("    ls      - list files in initramfs.\n");
    uart_puts("    cat     - print file content.\n");
    uart_puts("    memtest - run Lab3 allocator test.\n");
    
}

static void shell_hello(void) {
    uart_puts("Hello world.\n");
}

static void shell_info(void) {
    uart_puts("System information:\n");

    uart_puts("    OpenSBI specification version: ");
    uart_hex((unsigned long)sbi_get_spec_version());
    uart_puts("\n");

    uart_puts("    implementation ID: ");
    uart_hex((unsigned long)sbi_get_impl_id());
    uart_puts("\n");

    uart_puts("    implementation version: ");
    uart_hex((unsigned long)sbi_get_impl_version());
    uart_puts("\n");
}

static void shell_execute(const char *cmd) {
    if (strcmp_cmd(cmd, "help")) {
        shell_help();
    } else if (strcmp_cmd(cmd, "hello")) {
        shell_hello();
    } else if (strcmp_cmd(cmd, "info")) {
        shell_info();
    } else if (strcmp_cmd(cmd, "ls")) {
        if (initrd_start)
            initrd_list((const void *)initrd_start);
        else
            uart_puts("initrd not found\n");
    } else if (strncmp_simple(cmd, "cat ", 4) == 0) {
        if (initrd_start)
            initrd_cat((const void *)initrd_start, cmd + 4);
        else
            uart_puts("initrd not found\n");
    } else if (strcmp_cmd(cmd, "memtest")) {
        test_alloc_1();
    } else if (strcmp_cmd(cmd, "load")) {
        shell_load();
    } else if (cmd[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}

/* ---------- Kernel entry ---------- */

void start_kernel(const void *fdt) {
    boot_fdt = fdt;

    uart_set_base(0xd4017000UL);
    uart_set_config(2, 4);

    /*
     * Lab3 is first loaded to 0x00200000.
     * Relocate it to 0x20000000 so that future load commands can reuse
     * 0x00200000 for Lab4 / Lab5 / later kernels.
     */
    if ((unsigned long)_start < RELOC_ADDR) {
        uart_puts("\nRelocating kernel...");
        relocate_self(fdt);
        while (1) {}
    }

    char buf[128];
    int idx = 0;

    uart_puts("\nRelocated kernel running.\n\n");

    uart_puts("fdt ptr = ");
    uart_hex((unsigned long)fdt);
    uart_puts("\n");

    uart_init_from_dtb(fdt);
    initrd_init_from_dtb(fdt);

    mm_init_advanced(fdt, initrd_start, initrd_end);

    uart_puts("\nStarting kernel ...\n");
    uart_puts("Type help to get commands.\n\n");

    print_prompt();

    while (1) {
        char c = uart_getc();

        if (c == '\n') {
            uart_putc('\n');
            buf[idx] = '\0';
            shell_execute(buf);
            idx = 0;
            print_prompt();
        } else if (c == 8 || c == 127) {
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
