#define UART_BASE 0x10000000UL
#define UART_THR  (*(volatile unsigned char*)(UART_BASE + 0x0))
#define UART_LSR  (*(volatile unsigned char*)(UART_BASE + 0x5))
#define LSR_TDRQ  (1 << 5)

static void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');
    while ((UART_LSR & LSR_TDRQ) == 0)
        ;
    UART_THR = c;
}

static void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void start_kernel(void) {
    uart_puts("Hello from loaded kernel\n");
    while (1) { }
}
