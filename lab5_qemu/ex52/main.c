extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);
extern void video_init();
extern void video_bmp_display(unsigned int* bmp_image, int width, int height);

#define TIME_FREQ 10000000

static inline unsigned long read_time(void) {
    unsigned long t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

int usleep(unsigned int usec) {
    unsigned long start;
    unsigned long now;
    unsigned long ticks;

    asm volatile("rdtime %0" : "=r"(start));

    ticks = ((unsigned long)usec * TIME_FREQ) / 1000000UL;

    do {
        asm volatile("rdtime %0" : "=r"(now));
    } while (now - start < ticks);

    return 0;
}

void display_video() {
#include "bird.h"

    while (1) {
        for (int f = 0; f < FRAME_COUNT; f++) {
            unsigned int* frame = frames + (f * FRAME_WIDTH * FRAME_HEIGHT);
            video_bmp_display(frame, FRAME_WIDTH, FRAME_HEIGHT);
            usleep(50000);
        }
    }
}

void start_kernel() {
    uart_puts("\nStarting kernel ...\n");
    video_init();
    display_video();
}
