#ifndef MM_H
#define MM_H

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096UL
#define MAX_ORDER 10
#define MAX_ALLOC_SIZE (PAGE_SIZE << MAX_ORDER)

void mm_init_advanced(const void *fdt,
                      unsigned long initrd_start,
                      unsigned long initrd_end);

void *allocate(size_t size);
void free(void *ptr);
void test_alloc_1(void);

#endif
