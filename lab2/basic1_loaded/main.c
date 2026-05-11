#define UART_BASE 0xD4017000UL
#define UART_REG(off) (UART_BASE + ((off) << 2))

#define UART_THR 0
#define UART_LSR 5

#define LSR_TDRQ  (1 << 5)

static unsigned int uart_read_reg(int off) {
    return *(volatile unsigned int *)UART_REG(off);
}

static void uart_write_reg(int off, unsigned int val) {
    *(volatile unsigned int *)UART_REG(off) = val;
}

static void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((uart_read_reg(UART_LSR) & LSR_TDRQ) == 0)
        ;

    uart_write_reg(UART_THR, (unsigned int)c);
}

static void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void start_kernel(void) {
    uart_puts("Hello from loaded kernel on RV2\n");
    while (1) { }
}
