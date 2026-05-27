/* Orange Pi RV2 framebuffer driver implementation. */

#include <stddef.h>
#include <stdint.h>

extern void *memcpy(void *dst, const void *src, size_t n);

#define FB_BASE   0x7f700000UL
#define FB_WIDTH  1920
#define FB_HEIGHT 1080

#define CACHE_BLOCK_SIZE 64

#define cbo_flush(start)                         \
    ({                                           \
        asm volatile("mv a0, %0\n\t"             \
                     ".word 0x0025200F"          \
                     :                           \
                     : "r"(start)                \
                     : "memory", "a0");         \
    })

static void flush_dcache(void *addr, unsigned long len)
{
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + len;

    __sync_synchronize();
    for (unsigned long line = start; line < end; line += CACHE_BLOCK_SIZE)
        cbo_flush(line);
    __sync_synchronize();
}

void video_init(void)
{
    /* U-Boot initializes the RV2 framebuffer before entering the kernel. */
}

void video_bmp_display(unsigned int *bmp_image, int width, int height)
{
    unsigned int *fb = (unsigned int *)FB_BASE;
    int start_x = (FB_WIDTH - width) / 2;
    int start_y = (FB_HEIGHT - height) / 2;

    for (int y = 0; y < height; y++)
    {
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        memcpy(dst, bmp_image + y * width, (size_t)width * sizeof(unsigned int));
        flush_dcache(dst, (unsigned long)width * sizeof(unsigned int));
    }
}
