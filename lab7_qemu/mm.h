#pragma once
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_ORDER 10
#define MAX_ALLOC_SIZE (PAGE_SIZE << MAX_ORDER)

void mm_init_advanced(const void *fdt,
                      unsigned long initrd_start,
                      unsigned long initrd_end);

void *allocate(size_t size);
void free(void *ptr);
void retain_page(void *ptr);
int page_refcount(void *ptr);

void test_alloc_1(void);
