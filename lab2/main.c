extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);
extern unsigned char uart_getb(void);
extern void uart_putb(unsigned char c);
extern void uart_set_base(unsigned long base);
extern void uart_set_config(int reg_shift, int reg_io_width);

#include <stdint.h>
#include <stddef.h>

#define SBI_EXT_SET_TIMER 0x0
#define SBI_EXT_SHUTDOWN  0x8
#define SBI_EXT_BASE      0x10
#define LOAD_ADDR ((unsigned char*)0x00200000UL)
#define RELOC_ADDR 0x20000000UL
#define KERNEL_ENTRY 0x80200000UL

#define BOOT_MAGIC 0x544F4F42UL

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

extern char _start[];
extern char _end[];
static const void *boot_fdt;

void start_kernel(const void *fdt);

static void relocate_self(const void *fdt) {
    unsigned char *src = (unsigned char *)_start;
    unsigned char *dst = (unsigned char *)RELOC_ADDR;
    unsigned long size = (unsigned long)(_end - _start);

    for (unsigned long i = 0; i < size; i++)
        dst[i] = src[i];

    unsigned long new_sp = RELOC_ADDR + ((unsigned long)_end - (unsigned long)_start);
    asm volatile("mv sp, %0" :: "r"(new_sp));

    void (*entry)(const void *) =
        (void (*)(const void *))(RELOC_ADDR + ((unsigned long)start_kernel - (unsigned long)_start));

    entry(fdt);
}

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

static const void *initrd_start = 0;
static const void *initrd_end = 0;

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
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

static inline const void* align_up(const void* ptr, size_t align) {
    return (const void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
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

static void uart_put_uint(unsigned int x) {
    char buf[16];
    int i = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }

    while (x > 0) {
        buf[i++] = '0' + (x % 10);
        x /= 10;
    }

    while (i > 0)
        uart_putc(buf[--i]);
}

int fdt_path_offset(const void* fdt, const char* path) {
    const struct fdt_header* hdr = (const struct fdt_header*)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed)
        return -1;

    const char* struct_base = (const char*)fdt + bswap32(hdr->off_dt_struct);
    const char* p = struct_base;
    char curpath[1024];
    curpath[0] = '\0';
    size_t pathlen_stack[128];
    int depth = 0;

    while (1) {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32(*(const uint32_t*)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE) {
            const char* name = p;
            size_t oldlen = strlen_simple(curpath);
            pathlen_stack[depth++] = oldlen;

            if (oldlen == 0) {
                strcpy_simple(curpath, "/");
            } else {
                if (strcmp_full(curpath, "/") != 0)
    	            strcat_simple(curpath, "/");
		strcat_simple(curpath, name);
            }

            /* path match:
             * allow "/memory" to match "/memory@80000000"
             */
            const char *a = curpath, *b = path;
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

            p = (const char*)align_up(p + strlen_simple(name) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth > 0) {
                size_t oldlen = pathlen_stack[--depth];
                curpath[oldlen] = '\0';
            }
        } else if (tag == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t*)p);
            p += 4; /* len */
            p += 4; /* nameoff */
            p = (const char*)align_up(p + len, 4);
        } else if (tag == FDT_NOP) {
            /* do nothing */
        } else if (tag == FDT_END) {
            break;
        } else {
            return -1;
        }
    }

    return -1;
}

const void* fdt_getprop(const void* fdt,
                        int nodeoffset,
                        const char* name,
                        int* lenp) {
    const struct fdt_header* hdr = (const struct fdt_header*)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed)
        return NULL;

    const char* struct_base = (const char*)fdt + bswap32(hdr->off_dt_struct);
    const char* strings_base = (const char*)fdt + bswap32(hdr->off_dt_strings);
    const char* p = struct_base + nodeoffset;

    if (bswap32(*(const uint32_t*)p) != FDT_BEGIN_NODE)
        return NULL;
    p += 4;

    /* skip node name */
    p = (const char*)align_up(p + strlen_simple(p) + 1, 4);

    int depth = 0;

    while (1) {
        uint32_t tag = bswap32(*(const uint32_t*)p);
        p += 4;

        if (tag == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t*)p);
            p += 4;
            uint32_t nameoff = bswap32(*(const uint32_t*)p);
            p += 4;

            const char* prop_name = strings_base + nameoff;
            const void* prop_data = p;
            
            if (depth == 0 && strcmp_full(prop_name, name) == 0) {
    		if (lenp)
        	    *lenp = (int)len;
    	        return prop_data;
	    }

	    p = (const char*)align_up(p + len, 4);
        } else if (tag == FDT_BEGIN_NODE) {
            depth++;
            p = (const char*)align_up(p + strlen_simple(p) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth == 0)
                break;
            depth--;
        } else if (tag == FDT_NOP) {
            /* do nothing */
        } else if (tag == FDT_END) {
            break;
        } else {
            return NULL;
        }
    }

    return NULL;
}

static void uart_init_from_dtb(const void *fdt) {
    int len;

    int offset = fdt_path_offset(fdt, "/soc/serial@d4017000");
    if (offset < 0)
        offset = fdt_path_offset(fdt, "/soc/serial");
    if (offset < 0)
        offset = fdt_path_offset(fdt, "/soc/uart");

    if (offset < 0) {
        uart_puts("fdt_path_offset(uart) failed\n");
        return;
    }

    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, offset, "reg", &len);
    if (!reg || len < 16) {
        uart_puts("fdt_getprop(reg) failed\n");
        return;
    }

    unsigned long base =
        ((unsigned long)bswap32(reg[0]) << 32) |
        (unsigned long)bswap32(reg[1]);

    const uint32_t *shift = (const uint32_t *)fdt_getprop(fdt, offset, "reg-shift", &len);
    int reg_shift = 0;
    if (shift && len >= 4)
        reg_shift = (int)bswap32(shift[0]);

    const uint32_t *width = (const uint32_t *)fdt_getprop(fdt, offset, "reg-io-width", &len);
    int reg_width = 1;
    if (width && len >= 4)
        reg_width = (int)bswap32(width[0]);

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

static unsigned long fdt_read_addr(const void *prop, int len) {
    const uint32_t *p = (const uint32_t *)prop;

    if (len >= 8) {
        return ((unsigned long)bswap32(p[0]) << 32) |
               (unsigned long)bswap32(p[1]);
    } else if (len >= 4) {
        return (unsigned long)bswap32(p[0]);
    }

    return 0;
}

static void initrd_init_from_dtb(const void *fdt) {
    int len;
    int offset = fdt_path_offset(fdt, "/chosen");
    if (offset < 0) {
        uart_puts("fdt_path_offset(/chosen) failed\n");
        return;
    }

    const void *startp = fdt_getprop(fdt, offset, "linux,initrd-start", &len);
    if (!startp) {
        uart_puts("fdt_getprop(initrd-start) failed\n");
        return;
    }
    initrd_start = (const void *)fdt_read_addr(startp, len);

    const void *endp = fdt_getprop(fdt, offset, "linux,initrd-end", &len);
    if (!endp) {
        uart_puts("fdt_getprop(initrd-end) failed\n");
        return;
    }
    initrd_end = (const void *)fdt_read_addr(endp, len);

    uart_puts("initrd start = ");
    uart_hex((unsigned long)initrd_start);
    uart_puts("\n");

    uart_puts("initrd end   = ");
    uart_hex((unsigned long)initrd_end);
    uart_puts("\n");
}

void initrd_list(const void* rd) {
    const char* p = (const char*)rd;

    while (1) {
        const struct cpio_t* hdr = (const struct cpio_t*)p;

        if (strncmp_simple(hdr->magic, "070701", 6) != 0) {
            uart_puts("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);

        const char* name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            return;

        uart_put_uint((unsigned int)filesize);
        uart_putc(' ');
        uart_puts(name);
        uart_putc('\n');

        const char* data = p + align_int(sizeof(struct cpio_t) + namesize, 4);
        p = data + align_int(filesize, 4);
    }
}

void initrd_cat(const void* rd, const char* filename) {
    const char* p = (const char*)rd;

    while (1) {
        const struct cpio_t* hdr = (const struct cpio_t*)p;

        if (strncmp_simple(hdr->magic, "070701", 6) != 0) {
            uart_puts("invalid cpio archive\n");
            return;
        }

        int namesize = hextoi_simple(hdr->namesize, 8);
        int filesize = hextoi_simple(hdr->filesize, 8);

        const char* name = p + sizeof(struct cpio_t);

        if (strcmp_full(name, "TRAILER!!!") == 0)
            break;

        const char* data = p + align_int(sizeof(struct cpio_t) + namesize, 4);

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
    register unsigned long a0 asm("a0") = (unsigned long)arg0;
    register unsigned long a1 asm("a1") = (unsigned long)arg1;
    register unsigned long a2 asm("a2") = (unsigned long)arg2;
    register unsigned long a3 asm("a3") = (unsigned long)arg3;
    register unsigned long a4 asm("a4") = (unsigned long)arg4;
    register unsigned long a5 asm("a5") = (unsigned long)arg5;
    register unsigned long a6 asm("a6") = (unsigned long)fid;
    register unsigned long a7 asm("a7") = (unsigned long)ext;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    ret.error = a0;
    ret.value = a1;
    return ret;
}

long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_SPEC_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_ID,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_probe_extension(int extid) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_PROBE_EXT,
                     extid, 0, 0, 0, 0, 0).value;
}

static int strcmp_simple(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void print_prompt(void) {
    uart_puts("Bootloader> ");
}

static void shell_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("    help  - show all commands.\n");
    uart_puts("    hello - print Hello world.\n");
    uart_puts("    info  - print system info.\n");
    uart_puts("    load  - load a kernel over UART.\n");
    uart_puts("    ls    - list files in initramfs.\n");
    uart_puts("    cat   - print file content.\n");
}

static void shell_hello(void) {
    uart_puts("Hello world.\n");
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

static void shell_execute(const char* cmd) {
    if (strcmp_simple(cmd, "help")) {
        shell_help();
    } else if (strcmp_simple(cmd, "hello")) {
        shell_hello();
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
        const char *name = cmd + 4;
        if (initrd_start)
            initrd_cat(initrd_start, name);
        else
            uart_puts("initrd not found\n");
    } else if (cmd[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}

void start_kernel(const void *fdt) {
    boot_fdt = fdt;	
    char buf[128];
    int idx = 0;
    
    
    if ((unsigned long)_start < RELOC_ADDR) {
        uart_puts("Relocating bootloader...\n");
        relocate_self(fdt);
        while (1) {}
    }

    uart_puts("Relocated bootloader running.\n");

    uart_puts("dtb ptr = ");
    uart_hex((unsigned long)fdt);
    uart_puts("\n");

    uart_init_from_dtb(fdt);
    initrd_init_from_dtb(fdt);

    uart_puts("\nStarting kernel ...\n\n");
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

