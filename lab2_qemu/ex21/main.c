#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static inline uint32_t bswap32(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint64_t bswap64(uint64_t x) {
    return __builtin_bswap64(x);
}

static inline const void* align_up(const void* ptr, size_t align) {
    return (const void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

int fdt_path_offset(const void* fdt, const char* path) {
    const struct fdt_header* hdr = (const struct fdt_header*)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed)
        return -1;

    const char* struct_base = (const char*)fdt + bswap32(hdr->off_dt_struct);
    const char* p = struct_base;
    char curpath[1024] = "";
    size_t pathlen_stack[128];
    int depth = 0;

    while (1) {
        int nodeoff = (int)(p - struct_base);
        uint32_t tag = bswap32(*(const uint32_t*)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE) {
            const char* name = p;
            size_t oldlen = strlen(curpath);
            pathlen_stack[depth++] = oldlen;

            if (oldlen == 0) {
                strcpy(curpath, "/");
            } else {
                if (strcmp(curpath, "/") != 0)
                    strcat(curpath, "/");
                strcat(curpath, name);
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

            p = (const char*)align_up(p + strlen(name) + 1, 4);
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
    p = (const char*)align_up(p + strlen(p) + 1, 4);

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

            if (depth == 0 && strcmp(prop_name, name) == 0) {
                if (lenp)
                    *lenp = (int)len;
                return prop_data;
            }

            p = (const char*)align_up(p + len, 4);
        } else if (tag == FDT_BEGIN_NODE) {
            depth++;
            p = (const char*)align_up(p + strlen(p) + 1, 4);
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

int main() {
    /* Prepare the device tree blob */
    FILE* fp = fopen("qemu.dtb", "rb");
    if (!fp) {
        perror("fopen");
        return EXIT_FAILURE;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    void* fdt = malloc(sz);
    fseek(fp, 0, SEEK_SET);
    if (fread(fdt, 1, sz, fp) != sz) {
        fprintf(stderr, "Failed to read the device tree blob\n");
        free(fdt);
        fclose(fp);
        return EXIT_FAILURE;
    }
    fclose(fp);

    /* Find the node offset */
    int offset = fdt_path_offset(fdt, "/cpus/cpu@0/interrupt-controller");
    if (offset < 0) {
        fprintf(stderr, "fdt_path_offset\n");
        free(fdt);
        return EXIT_FAILURE;
    }

    /* Get the node property */
    int len;
    const void* prop = fdt_getprop(fdt, offset, "compatible", &len);
    if (!prop) {
        fprintf(stderr, "fdt_getprop\n");
        free(fdt);
        return EXIT_FAILURE;
    }
    printf("compatible: %.*s\n", len, (const char*)prop);

    offset = fdt_path_offset(fdt, "/memory");
    prop = fdt_getprop(fdt, offset, "reg", &len);
    const uint64_t* reg = (const uint64_t*)prop;
    printf("memory: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));

    offset = fdt_path_offset(fdt, "/chosen");
    prop = fdt_getprop(fdt, offset, "linux,initrd-start", &len);
    const uint64_t* initrd_start = (const uint64_t*)prop;
    printf("initrd-start: 0x%lx\n", bswap64(initrd_start[0]));

    free(fdt);
    return 0;
}
