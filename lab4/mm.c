#include "mm.h"
#include <stdint.h>
#include <stddef.h>

#define MM_BOOT_LOG 1


extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putc(char c);

extern int fdt_path_offset(const void *fdt, const char *path);
extern const void *fdt_getprop(const void *fdt,
                               int nodeoffset,
                               const char *name,
                               int *lenp);

extern char _start[];
extern char _end[];

#define PAGE_FREE_IN_BLOCK -1
#define PAGE_ALLOCATED     -2

#define CHUNK_CLASS_COUNT 8
#define MAX_RESERVED 64

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

struct page {
    int order;
    int refcount;
    struct page *next;
    struct page *prev;
};

struct chunk {
    struct chunk *next;
};

struct page_chunk_info {
    unsigned long chunk_size;
    unsigned long total_chunks;
    unsigned long free_chunks;
};

struct reserved_range {
    unsigned long start;
    unsigned long end;
};

struct fdt_header_mm {
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

static unsigned long mem_base;
static unsigned long mem_size;
static unsigned long num_pages;

static struct page *mem_map;
static struct page_chunk_info *chunk_info;

static struct page *free_area[MAX_ORDER + 1];
static struct chunk *chunk_free_list[CHUNK_CLASS_COUNT];

static struct reserved_range reserved[MAX_RESERVED];
static int reserved_count = 0;

static unsigned long startup_ptr;

static int mm_log_enabled = 0;

static const unsigned long chunk_sizes[CHUNK_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* ---------- Basic helpers ---------- */

static unsigned int bswap32_mm(unsigned int x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
}

static unsigned long align_up_ul(unsigned long x, unsigned long a) {
    return (x + a - 1) & ~(a - 1);
}

static unsigned long align_down_ul(unsigned long x, unsigned long a) {
    return x & ~(a - 1);
}

static size_t strlen_simple(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static const void *align_ptr(const void *ptr, size_t align) {
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static void print_dec(unsigned long x) {
    char buf[32];
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

static unsigned long read_cells(const uint32_t *p, int cells) {
    unsigned long v = 0;

    for (int i = 0; i < cells; i++)
        v = (v << 32) | bswap32_mm(p[i]);

    return v;
}

/* ---------- Page metadata O(1) mapping ---------- */

static unsigned long page_idx(struct page *p) {
    return (unsigned long)(p - mem_map);
}

static unsigned long page_addr(struct page *p) {
    return mem_base + page_idx(p) * PAGE_SIZE;
}

static struct page *addr_to_page(unsigned long addr) {
    unsigned long idx = (addr - mem_base) / PAGE_SIZE;
    return &mem_map[idx];
}

/* ---------- Free list ---------- */

static void list_add(struct page **head, struct page *p) {
    p->prev = 0;
    p->next = *head;

    if (*head)
        (*head)->prev = p;

    *head = p;

    if (!mm_log_enabled)
        return;

    uart_puts("[+] Add page ");
    print_dec(page_idx(p));
    uart_puts(" to order ");
    print_dec((unsigned long)p->order);
    uart_puts(". Range of pages: [");
    print_dec(page_idx(p));
    uart_puts(", ");
    print_dec(page_idx(p) + ((1UL << p->order) - 1));
    uart_puts("]\n");
}

static void list_remove(struct page **head, struct page *p) {
    if (p->prev)
        p->prev->next = p->next;
    else
        *head = p->next;

    if (p->next)
        p->next->prev = p->prev;

    p->next = 0;
    p->prev = 0;

    if (!mm_log_enabled)
        return;

    uart_puts("[-] Remove page ");
    print_dec(page_idx(p));
    uart_puts(" from order ");
    print_dec((unsigned long)p->order);
    uart_puts(". Range of pages: [");
    print_dec(page_idx(p));
    uart_puts(", ");
    print_dec(page_idx(p) + ((1UL << p->order) - 1));
    uart_puts("]\n");
}

/* ---------- Reserved ranges ---------- */

static void add_reserved(unsigned long start, unsigned long size) {
    if (size == 0 || reserved_count >= MAX_RESERVED)
        return;

    unsigned long end = start + size;

    reserved[reserved_count].start = align_down_ul(start, PAGE_SIZE);
    reserved[reserved_count].end = align_up_ul(end, PAGE_SIZE);

#if MM_BOOT_LOG
    uart_puts("[Reserve] Add reserved range [");
    uart_hex(reserved[reserved_count].start);
    uart_puts(", ");
    uart_hex(reserved[reserved_count].end);
    uart_puts(")\n");
#endif

    reserved_count++;
}

static int overlap_reserved(unsigned long start, unsigned long end) {
    for (int i = 0; i < reserved_count; i++) {
        if (!(end <= reserved[i].start || start >= reserved[i].end))
            return 1;
    }

    return 0;
}

static void sort_reserved_ranges(void) {
    for (int i = 1; i < reserved_count; i++) {
        struct reserved_range key = reserved[i];
        int j = i - 1;

        while (j >= 0 && reserved[j].start > key.start) {
            reserved[j + 1] = reserved[j];
            j--;
        }

        reserved[j + 1] = key;
    }
}

static void normalize_reserved_ranges(void) {
    unsigned long mem_end = mem_base + mem_size;
    int out = 0;

    for (int i = 0; i < reserved_count; i++) {
        unsigned long start = reserved[i].start;
        unsigned long end = reserved[i].end;

        if (end <= mem_base || start >= mem_end)
            continue;

        if (start < mem_base)
            start = mem_base;
        if (end > mem_end)
            end = mem_end;

        start = align_down_ul(start, PAGE_SIZE);
        end = align_up_ul(end, PAGE_SIZE);

        if (start < mem_base)
            start = mem_base;
        if (end > mem_end)
            end = mem_end;

        if (end <= start)
            continue;

        reserved[out].start = start;
        reserved[out].end = end;
        out++;
    }

    reserved_count = out;

    if (reserved_count == 0)
        return;

    sort_reserved_ranges();

    out = 0;
    for (int i = 0; i < reserved_count; i++) {
        if (out == 0) {
            reserved[out++] = reserved[i];
            continue;
        }

        if (reserved[i].start <= reserved[out - 1].end) {
            if (reserved[i].end > reserved[out - 1].end)
                reserved[out - 1].end = reserved[i].end;
        } else {
            reserved[out++] = reserved[i];
        }
    }

    reserved_count = out;
}

static void add_free_block(unsigned long pfn, unsigned int order) {
    if (pfn >= num_pages)
        return;

    if (order > MAX_ORDER)
        return;

    if (pfn + (1UL << order) > num_pages)
        return;

    struct page *p = &mem_map[pfn];

    p->order = order;
    p->refcount = 0;
    p->next = 0;
    p->prev = 0;

    list_add(&free_area[order], p);
}

static void add_free_range(unsigned long start, unsigned long end) {
    unsigned long phys_end = mem_base + mem_size;

    if (end <= mem_base || start >= phys_end)
        return;

    if (start < mem_base)
        start = mem_base;

    if (end > phys_end)
        end = phys_end;

    start = align_up_ul(start, PAGE_SIZE);
    end = align_down_ul(end, PAGE_SIZE);

    if (end <= start)
        return;

    unsigned long pfn = (start - mem_base) / PAGE_SIZE;
    unsigned long end_pfn = (end - mem_base) / PAGE_SIZE;

    unsigned long guard = 0;

    while (pfn < end_pfn) {
        if (++guard > num_pages + 1024) {
   
            return;
        }

        unsigned int order = 0;

        while (order < MAX_ORDER) {
            unsigned long next_pages = 1UL << (order + 1);

            if ((pfn & (next_pages - 1)) != 0)
                break;

            if (pfn + next_pages > end_pfn)
                break;

            order++;
        }

        add_free_block(pfn, order);
        pfn += 1UL << order;
    }
}

/* ---------- Startup allocator ---------- */

static void *startup_alloc(unsigned long size, unsigned long align) {
    unsigned long p = align_up_ul(startup_ptr, align);

    while (p + size <= mem_base + mem_size) {
        if (!overlap_reserved(p, p + size)) {
            startup_ptr = p + size;
            add_reserved(p, size);

#if MM_BOOT_LOG
            uart_puts("[Startup] Allocate ");
            uart_hex(p);
            uart_puts(" size ");
            uart_hex(size);
            uart_puts("\n");
#endif

            return (void *)p;
        }

        int moved = 0;

        for (int i = 0; i < reserved_count; i++) {
            if (!(p + size <= reserved[i].start || p >= reserved[i].end)) {
                p = align_up_ul(reserved[i].end, align);
                moved = 1;
                break;
            }
        }

        if (!moved)
            break;
    }

    uart_puts("[Startup] allocation failed\n");
    return 0;
}

/* ---------- DTB parsing ---------- */

static void parse_memory_region(const void *fdt) {
    int len;

    int root = fdt_path_offset(fdt, "/");
    int memory = fdt_path_offset(fdt, "/memory");

    int addr_cells = 2;
    int size_cells = 2;

    const uint32_t *prop;

    prop = (const uint32_t *)fdt_getprop(fdt, root, "#address-cells", &len);
    if (prop && len >= 4)
        addr_cells = (int)bswap32_mm(prop[0]);

    prop = (const uint32_t *)fdt_getprop(fdt, root, "#size-cells", &len);
    if (prop && len >= 4)
        size_cells = (int)bswap32_mm(prop[0]);

    prop = (const uint32_t *)fdt_getprop(fdt, memory, "reg", &len);
    if (!prop) {
        uart_puts("[MM] failed to parse /memory/reg\n");
        return;
    }

    mem_base = read_cells(prop, addr_cells);
    mem_size = read_cells(prop + addr_cells, size_cells);
    num_pages = mem_size / PAGE_SIZE;

    #if MM_BOOT_LOG
	uart_puts("[MM] memory base = ");
	uart_hex(mem_base);
	uart_puts("\n");

	uart_puts("[MM] memory size = ");
	uart_hex(mem_size);
	uart_puts("\n");

	uart_puts("[MM] num pages = ");
	print_dec(num_pages);
	uart_puts("\n");
	#endif
}

static unsigned long read_be64_cells(const uint32_t *p) {
    unsigned long hi = bswap32_mm(p[0]);
    unsigned long lo = bswap32_mm(p[1]);
    return (hi << 32) | lo;
}

static void parse_fdt_reserve_map(const void *fdt) {
    const struct fdt_header_mm *hdr = (const struct fdt_header_mm *)fdt;
    const uint32_t *p =
        (const uint32_t *)((const char *)fdt + bswap32_mm(hdr->off_mem_rsvmap));

    while (1) {
        unsigned long addr = read_be64_cells(p);
        unsigned long size = read_be64_cells(p + 2);

        if (addr == 0 && size == 0)
            break;

        add_reserved(addr, size);
        p += 4;
    }
}

static void parse_reserved_memory_node(const void *fdt) {
    int reserved_node = fdt_path_offset(fdt, "/reserved-memory");
    if (reserved_node < 0)
        return;

    int len;
    int root = fdt_path_offset(fdt, "/");

    int addr_cells = 2;
    int size_cells = 2;

    const uint32_t *prop;

    prop = (const uint32_t *)fdt_getprop(fdt, root, "#address-cells", &len);
    if (prop && len >= 4)
        addr_cells = (int)bswap32_mm(prop[0]);

    prop = (const uint32_t *)fdt_getprop(fdt, root, "#size-cells", &len);
    if (prop && len >= 4)
        size_cells = (int)bswap32_mm(prop[0]);

    prop = (const uint32_t *)fdt_getprop(fdt, reserved_node, "#address-cells", &len);
    if (prop && len >= 4)
        addr_cells = (int)bswap32_mm(prop[0]);

    prop = (const uint32_t *)fdt_getprop(fdt, reserved_node, "#size-cells", &len);
    if (prop && len >= 4)
        size_cells = (int)bswap32_mm(prop[0]);

    const struct fdt_header_mm *hdr = (const struct fdt_header_mm *)fdt;
    const char *struct_base = (const char *)fdt + bswap32_mm(hdr->off_dt_struct);
    const char *p = struct_base + reserved_node;

    if (bswap32_mm(*(const uint32_t *)p) != FDT_BEGIN_NODE)
        return;

    p += 4;
    p = (const char *)align_ptr(p + strlen_simple(p) + 1, 4);

    int depth = 0;

    while (1) {
        const char *tag_addr = p;
        uint32_t tag = bswap32_mm(*(const uint32_t *)p);
        p += 4;

        if (tag == FDT_BEGIN_NODE) {
            int nodeoff = (int)(tag_addr - struct_base);

            if (depth == 0) {
                const uint32_t *reg =
                    (const uint32_t *)fdt_getprop(fdt, nodeoff, "reg", &len);

                if (reg) {
                    int entry_cells = addr_cells + size_cells;
                    int entry_bytes = entry_cells * 4;
                    int count = len / entry_bytes;

                    for (int i = 0; i < count; i++) {
                        const uint32_t *entry = reg + i * entry_cells;
                        unsigned long start = read_cells(entry, addr_cells);
                        unsigned long size = read_cells(entry + addr_cells, size_cells);
                        add_reserved(start, size);
                    }
                }
            }

            depth++;
            p = (const char *)align_ptr(p + strlen_simple(p) + 1, 4);
        } else if (tag == FDT_END_NODE) {
            if (depth == 0)
                break;
            depth--;
        } else if (tag == FDT_PROP) {
            uint32_t prop_len = bswap32_mm(*(const uint32_t *)p);
            p += 4;
            p += 4;
            p = (const char *)align_ptr(p + prop_len, 4);
        } else if (tag == FDT_NOP) {
            /* nothing */
        } else if (tag == FDT_END) {
            break;
        } else {
            break;
        }
    }
}

/* ---------- Buddy allocator ---------- */

static struct page *get_buddy(struct page *p, unsigned int order) {
    unsigned long idx = page_idx(p);
    unsigned long buddy_idx = idx ^ (1UL << order);

    if (buddy_idx >= num_pages)
        return 0;

    if (mm_log_enabled) {
        uart_puts("[*] Buddy found! buddy idx: ");
        print_dec(buddy_idx);
        uart_puts(" for page ");
        print_dec(idx);
        uart_puts(" with order ");
        print_dec(order);
        uart_puts("\n");
    }

    return &mem_map[buddy_idx];
}

static struct page *alloc_pages(unsigned int order) {
    unsigned int current_order = order;

    while (current_order <= MAX_ORDER && free_area[current_order] == 0)
        current_order++;

    if (current_order > MAX_ORDER)
        return 0;

    struct page *p = free_area[current_order];
    list_remove(&free_area[current_order], p);

    while (current_order > order) {
        current_order--;

        struct page *buddy = get_buddy(p, current_order);

        p->order = current_order;
        buddy->order = current_order;
        buddy->refcount = 0;

        list_add(&free_area[current_order], buddy);
    }

    p->order = order;
    p->refcount = 1;

    if (mm_log_enabled) {
        uart_puts("[Page] Allocate ");
        uart_hex(page_addr(p));
        uart_puts(" at order ");
        print_dec(order);
        uart_puts(", page ");
        print_dec(page_idx(p));
        uart_puts(". Next address at order ");
        print_dec(order);
        uart_puts(": ");
        uart_hex(page_addr(p) + (PAGE_SIZE << order));
        uart_puts("\n");
    }

    return p;
}

static void clear_chunk_info(unsigned long idx) {
    chunk_info[idx].chunk_size = 0;
    chunk_info[idx].total_chunks = 0;
    chunk_info[idx].free_chunks = 0;
}

static void free_pages_internal(struct page *p) {
    unsigned int order = p->order;

    p->refcount = 0;

    while (order < MAX_ORDER) {
        struct page *buddy = get_buddy(p, order);

        if (!buddy)
            break;

        if (buddy->refcount != 0 || buddy->order != (int)order)
            break;

        list_remove(&free_area[order], buddy);

        if (buddy < p)
            p = buddy;

        order++;
        p->order = order;
    }

    p->order = order;
    p->refcount = 0;

    clear_chunk_info(page_idx(p));

    list_add(&free_area[order], p);

    if (mm_log_enabled) {
        uart_puts("[Page] Free ");
        uart_hex(page_addr(p));
        uart_puts(" and add back to order ");
        print_dec(order);
        uart_puts(", page ");
        print_dec(page_idx(p));
        uart_puts(". Next address at order ");
        print_dec(order);
        uart_puts(": ");
        uart_hex(page_addr(p) + (PAGE_SIZE << order));
        uart_puts("\n");
    }
}

/* ---------- Dynamic allocator ---------- */

static unsigned int size_to_order(size_t size) {
    unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned int order = 0;
    unsigned long block_pages = 1;

    while (block_pages < pages) {
        block_pages <<= 1;
        order++;
    }

    return order;
}

static int get_chunk_class(size_t size) {
    for (int i = 0; i < CHUNK_CLASS_COUNT; i++) {
        if (size <= chunk_sizes[i])
            return i;
    }

    return -1;
}

static void remove_page_chunks_from_freelist(unsigned long page_base,
                                             int class_id) {
    struct chunk *cur = chunk_free_list[class_id];
    struct chunk *prev = 0;

    while (cur) {
        struct chunk *next = cur->next;
        unsigned long addr = (unsigned long)cur;

        if (addr >= page_base && addr < page_base + PAGE_SIZE) {
            if (prev)
                prev->next = next;
            else
                chunk_free_list[class_id] = next;
        } else {
            prev = cur;
        }

        cur = next;
    }
}

static void *alloc_chunk(size_t size) {
    int class_id = get_chunk_class(size);
    if (class_id < 0)
        return 0;

    unsigned long chunk_size = chunk_sizes[class_id];

    if (!chunk_free_list[class_id]) {
        struct page *p = alloc_pages(0);
        if (!p)
            return 0;

        unsigned long base = page_addr(p);
        unsigned long idx = page_idx(p);
        unsigned long count = PAGE_SIZE / chunk_size;

        chunk_info[idx].chunk_size = chunk_size;
        chunk_info[idx].total_chunks = count;
        chunk_info[idx].free_chunks = count;

        for (unsigned long i = 0; i < count; i++) {
            struct chunk *c = (struct chunk *)(base + i * chunk_size);
            c->next = chunk_free_list[class_id];
            chunk_free_list[class_id] = c;
        }
    }

    struct chunk *c = chunk_free_list[class_id];
    chunk_free_list[class_id] = c->next;

    unsigned long page_base = ((unsigned long)c) & ~(PAGE_SIZE - 1);
    struct page *owner = addr_to_page(page_base);
    unsigned long owner_idx = page_idx(owner);

    if (chunk_info[owner_idx].free_chunks > 0)
        chunk_info[owner_idx].free_chunks--;

    if (mm_log_enabled) {
        uart_puts("[Chunk] Allocate ");
        uart_hex((unsigned long)c);
        uart_puts(" at chunk size ");
        print_dec(chunk_size);
        uart_puts("\n");
    }

    return c;
}

static void free_chunk(void *ptr) {
    unsigned long addr = (unsigned long)ptr;
    unsigned long page_base = addr & ~(PAGE_SIZE - 1);
    struct page *p = addr_to_page(page_base);
    unsigned long idx = page_idx(p);

    unsigned long chunk_size = chunk_info[idx].chunk_size;
    int class_id = get_chunk_class(chunk_size);

    if (class_id < 0)
        return;

    if (chunk_info[idx].free_chunks >= chunk_info[idx].total_chunks)
        return;

    struct chunk *c = (struct chunk *)ptr;
    c->next = chunk_free_list[class_id];
    chunk_free_list[class_id] = c;

    chunk_info[idx].free_chunks++;

    if (mm_log_enabled) {
        uart_puts("[Chunk] Free ");
        uart_hex(addr);
        uart_puts(" at chunk size ");
        print_dec(chunk_size);
        uart_puts("\n");
    }

    if (chunk_info[idx].free_chunks == chunk_info[idx].total_chunks) {
        remove_page_chunks_from_freelist(page_base, class_id);
        clear_chunk_info(idx);
        free_pages_internal(p);
    }
}

/* ---------- Public API ---------- */

void *allocate(size_t size) {
    if (size == 0 || size > MAX_ALLOC_SIZE)
        return 0;

    if (size <= chunk_sizes[CHUNK_CLASS_COUNT - 1])
        return alloc_chunk(size);

    unsigned int order = size_to_order(size);

    if (order > MAX_ORDER)
        return 0;

    struct page *p = alloc_pages(order);

    if (!p)
        return 0;

    return (void *)page_addr(p);
}

void free(void *ptr) {
    if (!ptr)
        return;

    unsigned long addr = (unsigned long)ptr;

    if (addr < mem_base || addr >= mem_base + mem_size)
        return;

    unsigned long page_base = addr & ~(PAGE_SIZE - 1);
    struct page *p = addr_to_page(page_base);

    if (chunk_info[page_idx(p)].chunk_size != 0) {
        free_chunk(ptr);
    } else {
        free_pages_internal(p);
    }
}

void mm_init_advanced(const void *fdt,
                      unsigned long initrd_start_addr,
                      unsigned long initrd_end_addr) {
    const struct fdt_header_mm *hdr = (const struct fdt_header_mm *)fdt;

    reserved_count = 0;
    mm_log_enabled = 1;

    parse_memory_region(fdt);

	add_reserved((unsigned long)fdt, bswap32_mm(hdr->totalsize));

	parse_fdt_reserve_map(fdt);

	parse_reserved_memory_node(fdt);

	#define KERNEL_STACK_RESERVE_SIZE (128 * 1024UL)

	unsigned long kernel_start = (unsigned long)_start;
	unsigned long kernel_end = (unsigned long)_end;

	add_reserved(kernel_start, kernel_end - kernel_start);
	add_reserved(kernel_end, KERNEL_STACK_RESERVE_SIZE);

	if (initrd_start_addr && initrd_end_addr > initrd_start_addr) {
		add_reserved(initrd_start_addr,
		             initrd_end_addr - initrd_start_addr);
	}

	startup_ptr = align_up_ul((unsigned long)_end, PAGE_SIZE);

	while (overlap_reserved(startup_ptr, startup_ptr + PAGE_SIZE)) {
		startup_ptr += PAGE_SIZE;
	}

	unsigned long mem_map_size = num_pages * sizeof(struct page);
	unsigned long chunk_info_size = num_pages * sizeof(struct page_chunk_info);

	mem_map = (struct page *)startup_alloc(mem_map_size, PAGE_SIZE);

	chunk_info = (struct page_chunk_info *)startup_alloc(chunk_info_size, PAGE_SIZE);

	if (!mem_map || !chunk_info) {
		return;
	}

	for (unsigned long i = 0; i < num_pages; i++) {
		mem_map[i].order = PAGE_ALLOCATED;
		mem_map[i].refcount = 1;
		mem_map[i].next = 0;
		mem_map[i].prev = 0;

		chunk_info[i].chunk_size = 0;
		chunk_info[i].total_chunks = 0;
		chunk_info[i].free_chunks = 0;
	}

	for (int i = 0; i <= MAX_ORDER; i++)
		free_area[i] = 0;

	for (int i = 0; i < CHUNK_CLASS_COUNT; i++)
		chunk_free_list[i] = 0;

	normalize_reserved_ranges();

	unsigned long cursor = mem_base;
	unsigned long mem_end = mem_base + mem_size;

	for (int i = 0; i < reserved_count; i++) {
		if (reserved[i].end <= cursor)
		    continue;

		if (reserved[i].start > cursor)
		    add_free_range(cursor, reserved[i].start);

		if (reserved[i].end > cursor)
		    cursor = reserved[i].end;
	}

	if (cursor < mem_end)
		add_free_range(cursor, mem_end);

	mm_log_enabled = 0;
}



void test_alloc_1(void) {
    mm_log_enabled = 1;
    uart_puts("Testing memory allocation...\n");

    char *ptr1 = (char *)allocate(4000);
    char *ptr2 = (char *)allocate(8000);
    char *ptr3 = (char *)allocate(4000);
    char *ptr4 = (char *)allocate(4000);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    uart_puts("Testing dynamic allocator...\n");

    char *kmem_ptr1 = (char *)allocate(16);
    char *kmem_ptr2 = (char *)allocate(32);
    char *kmem_ptr3 = (char *)allocate(64);
    char *kmem_ptr4 = (char *)allocate(128);
    char *kmem_ptr4_1 = (char *)allocate(256);
    char *kmem_ptr4_2 = (char *)allocate(512);
    char *kmem_ptr4_3 = (char *)allocate(1024);
    char *kmem_ptr4_4 = (char *)allocate(2048);

    free(kmem_ptr1);
    free(kmem_ptr2);
    free(kmem_ptr3);
    free(kmem_ptr4);
    free(kmem_ptr4_1);
    free(kmem_ptr4_2);
    free(kmem_ptr4_3);
    free(kmem_ptr4_4);

    char *kmem_ptr5 = (char *)allocate(16);
    char *kmem_ptr6 = (char *)allocate(32);

    free(kmem_ptr5);
    free(kmem_ptr6);

    void *kmem_ptr[102];

    for (int i = 0; i < 100; i++)
        kmem_ptr[i] = allocate(128);

    for (int i = 0; i < 100; i++)
        free(kmem_ptr[i]);

    char *kmem_ptr7 = (char *)allocate(MAX_ALLOC_SIZE + 1);

    if (kmem_ptr7 == 0) {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    } else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        free(kmem_ptr7);
    }

    mm_log_enabled = 0;
}
