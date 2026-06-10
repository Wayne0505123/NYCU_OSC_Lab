/* Framebuffer driver implementation.
 *
 * QEMU builds use ramfb through fw_cfg DMA.  Non-QEMU builds keep the
 * Orange Pi RV2 framebuffer path initialized by U-Boot.
 */

#include <stddef.h>
#include <stdint.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);
extern void uart_puts(const char *s);

#ifndef QEMU_VIDEO
#define QEMU_VIDEO 0
#endif

#define PAGE_OFFSET UINT64_C(0xffffffc000000000)

static inline int is_kernel_va(uint64_t addr)
{
    return addr >= PAGE_OFFSET;
}

static inline uint64_t virt_to_phys_addr(uint64_t va)
{
    return is_kernel_va(va) ? va - PAGE_OFFSET : va;
}

#if QEMU_VIDEO

#define QEMU_FW_CFG_BASE UINT64_C(0x10100000)
#define QEMU_FW_CFG_DATA (PAGE_OFFSET + QEMU_FW_CFG_BASE + 0)
#define QEMU_FW_CFG_SELECTOR (PAGE_OFFSET + QEMU_FW_CFG_BASE + 8)
#define QEMU_FW_CFG_DMA (PAGE_OFFSET + QEMU_FW_CFG_BASE + 16)

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_ID 0x0001
#define FW_CFG_FILE_DIR 0x0019

#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE 0x10

#define DRM_FORMAT_XRGB8888 0x34325258U
#ifndef QEMU_FB_WIDTH
#define QEMU_FB_WIDTH 640
#endif
#ifndef QEMU_FB_HEIGHT
#define QEMU_FB_HEIGHT 480
#endif
#define QEMU_FB_STRIDE (QEMU_FB_WIDTH * 4)

#define FW_CFG_FILE_ENTRY_SIZE 64
#define FW_CFG_FILE_SELECT_OFFSET 4
#define FW_CFG_FILE_NAME_OFFSET 8
#define FW_CFG_FILE_NAME_SIZE 56

struct fw_cfg_dma_access
{
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed));

struct ramfb_cfg
{
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

static unsigned int qemu_fb[QEMU_FB_WIDTH * QEMU_FB_HEIGHT]
    __attribute__((section(".bss"), aligned(4096)));
static struct fw_cfg_dma_access fw_dma __attribute__((aligned(16)));
static struct ramfb_cfg ramfb_cfg __attribute__((aligned(16)));
static int qemu_ramfb_ready;
static int qemu_fb_region_valid;
static int qemu_fb_last_x;
static int qemu_fb_last_y;
static int qemu_fb_last_w;
static int qemu_fb_last_h;

static uint16_t bswap16_video(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static uint32_t bswap32_video(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static uint64_t bswap64_video(uint64_t x)
{
    return ((uint64_t)bswap32_video((uint32_t)x) << 32) |
           bswap32_video((uint32_t)(x >> 32));
}

static void fw_cfg_select(uint16_t key)
{
    *(volatile uint16_t *)QEMU_FW_CFG_SELECTOR = bswap16_video(key);
}

static void fw_cfg_read_bytes(void *dst, size_t len)
{
    unsigned char *p = (unsigned char *)dst;

    for (size_t i = 0; i < len; i++)
        p[i] = *(volatile unsigned char *)QEMU_FW_CFG_DATA;
}

static uint16_t read_be16_buf(const unsigned char *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32_buf(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           p[3];
}

static int fw_cfg_has_signature(void)
{
    char sig[4];

    fw_cfg_select(FW_CFG_SIGNATURE);
    fw_cfg_read_bytes(sig, sizeof(sig));

    return sig[0] == 'Q' && sig[1] == 'E' &&
           sig[2] == 'M' && sig[3] == 'U';
}

static int name_equals(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }

    return *a == *b;
}

static int fw_cfg_find_file(const char *name, uint16_t *select_out)
{
    unsigned char count_buf[4];
    uint32_t count;

    fw_cfg_select(FW_CFG_FILE_DIR);
    fw_cfg_read_bytes(count_buf, sizeof(count_buf));
    count = read_be32_buf(count_buf);

    for (uint32_t i = 0; i < count; i++)
    {
        unsigned char raw[FW_CFG_FILE_ENTRY_SIZE];
        char *file_name = (char *)&raw[FW_CFG_FILE_NAME_OFFSET];

        fw_cfg_read_bytes(raw, sizeof(raw));
        raw[FW_CFG_FILE_NAME_OFFSET + FW_CFG_FILE_NAME_SIZE - 1] = '\0';

        if (name_equals(file_name, name))
        {
            *select_out = read_be16_buf(&raw[FW_CFG_FILE_SELECT_OFFSET]);
            return 0;
        }
    }

    return -1;
}

static int fw_cfg_dma_write(uint16_t select, const void *src, uint32_t len)
{
    uint32_t control = ((uint32_t)select << 16) |
                       FW_CFG_DMA_CTL_SELECT |
                       FW_CFG_DMA_CTL_WRITE;
    uint64_t dma_pa = virt_to_phys_addr((uint64_t)(uintptr_t)&fw_dma);
    uint64_t src_pa = virt_to_phys_addr((uint64_t)(uintptr_t)src);
    int guard = 1000000;

    fw_dma.control = bswap32_video(control);
    fw_dma.length = bswap32_video(len);
    fw_dma.address = bswap64_video(src_pa);

    __sync_synchronize();
    *(volatile uint64_t *)QEMU_FW_CFG_DMA = bswap64_video(dma_pa);
    __sync_synchronize();

    while (fw_dma.control && guard-- > 0)
        ;

    return fw_dma.control == 0 ? 0 : -1;
}

void video_init(void)
{
    unsigned char features_buf[4];
    uint32_t features;
    uint16_t ramfb_select;

    qemu_ramfb_ready = 0;

    if (!fw_cfg_has_signature())
        return;

    fw_cfg_select(FW_CFG_ID);
    fw_cfg_read_bytes(features_buf, sizeof(features_buf));
    features = ((uint32_t)features_buf[0]) |
               ((uint32_t)features_buf[1] << 8) |
               ((uint32_t)features_buf[2] << 16) |
               ((uint32_t)features_buf[3] << 24);
    if (!(features & 0x2))
    {
        uart_puts("[Video] QEMU fw_cfg DMA is unavailable\n");
        return;
    }

    if (fw_cfg_find_file("etc/ramfb", &ramfb_select) < 0)
    {
        uart_puts("[Video] QEMU ramfb device is missing\n");
        return;
    }

    memset(qemu_fb, 0, sizeof(qemu_fb));
    qemu_fb_region_valid = 0;

    ramfb_cfg.addr =
        bswap64_video(virt_to_phys_addr((uint64_t)(uintptr_t)qemu_fb));
    ramfb_cfg.fourcc = bswap32_video(DRM_FORMAT_XRGB8888);
    ramfb_cfg.flags = 0;
    ramfb_cfg.width = bswap32_video(QEMU_FB_WIDTH);
    ramfb_cfg.height = bswap32_video(QEMU_FB_HEIGHT);
    ramfb_cfg.stride = bswap32_video(QEMU_FB_STRIDE);

    if (fw_cfg_dma_write(ramfb_select, &ramfb_cfg, sizeof(ramfb_cfg)) < 0)
    {
        uart_puts("[Video] QEMU ramfb init failed\n");
        return;
    }

    qemu_ramfb_ready = 1;
}

void video_bmp_display(unsigned int *bmp_image, int width, int height)
{
    int copy_w;
    int copy_h;
    int src_x;
    int src_y;
    int dst_x;
    int dst_y;

    if (!qemu_ramfb_ready || !bmp_image || width <= 0 || height <= 0)
        return;

    copy_w = width < QEMU_FB_WIDTH ? width : QEMU_FB_WIDTH;
    copy_h = height < QEMU_FB_HEIGHT ? height : QEMU_FB_HEIGHT;
    src_x = width > QEMU_FB_WIDTH ? (width - QEMU_FB_WIDTH) / 2 : 0;
    src_y = height > QEMU_FB_HEIGHT ? (height - QEMU_FB_HEIGHT) / 2 : 0;
    dst_x = (QEMU_FB_WIDTH - copy_w) / 2;
    dst_y = (QEMU_FB_HEIGHT - copy_h) / 2;

    if (!qemu_fb_region_valid ||
        qemu_fb_last_x != dst_x ||
        qemu_fb_last_y != dst_y ||
        qemu_fb_last_w != copy_w ||
        qemu_fb_last_h != copy_h)
    {
        memset(qemu_fb, 0, sizeof(qemu_fb));
        qemu_fb_last_x = dst_x;
        qemu_fb_last_y = dst_y;
        qemu_fb_last_w = copy_w;
        qemu_fb_last_h = copy_h;
        qemu_fb_region_valid = 1;
    }

    for (int y = 0; y < copy_h; y++)
    {
        unsigned int *dst = qemu_fb + (dst_y + y) * QEMU_FB_WIDTH + dst_x;
        unsigned int *src = bmp_image + (src_y + y) * width + src_x;

        memcpy(dst, src, (size_t)copy_w * sizeof(unsigned int));
    }

    __sync_synchronize();
}

long video_fb_write(unsigned long offset, const void *buf, unsigned long len)
{
    unsigned long fb_size = (unsigned long)QEMU_FB_STRIDE * QEMU_FB_HEIGHT;

    if (!qemu_ramfb_ready || !buf)
        return -1;
    if (offset >= fb_size)
        return 0;
    if (len > fb_size - offset)
        len = fb_size - offset;

    memcpy((unsigned char *)qemu_fb + offset, buf, (size_t)len);
    return (long)len;
}

int video_fb_get_info(unsigned int *width, unsigned int *height,
                      unsigned int *bpp)
{
    if (!qemu_ramfb_ready || !width || !height || !bpp)
        return -1;

    *width = QEMU_FB_WIDTH;
    *height = QEMU_FB_HEIGHT;
    *bpp = 4;
    return 0;
}

#else

#define FB_BASE 0x7f700000UL
#define FB_WIDTH 1920
#define FB_HEIGHT 1080
#define FB_BPP 4

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
    unsigned int *fb = (unsigned int *)(PAGE_OFFSET + FB_BASE);
    int start_x = (FB_WIDTH - width) / 2;
    int start_y = (FB_HEIGHT - height) / 2;

    for (int y = 0; y < height; y++)
    {
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        memcpy(dst, bmp_image + y * width, (size_t)width * sizeof(unsigned int));
        flush_dcache(dst, (unsigned long)width * sizeof(unsigned int));
    }
}

long video_fb_write(unsigned long offset, const void *buf, unsigned long len)
{
    unsigned long fb_size = (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP;
    void *dst;

    if (!buf)
        return -1;
    if (offset >= fb_size)
        return 0;
    if (len > fb_size - offset)
        len = fb_size - offset;

    dst = (void *)(PAGE_OFFSET + FB_BASE + offset);
    memcpy(dst, buf, (size_t)len);
    flush_dcache(dst, len);
    return (long)len;
}

int video_fb_get_info(unsigned int *width, unsigned int *height,
                      unsigned int *bpp)
{
    if (!width || !height || !bpp)
        return -1;

    *width = FB_WIDTH;
    *height = FB_HEIGHT;
    *bpp = FB_BPP;
    return 0;
}

#endif
