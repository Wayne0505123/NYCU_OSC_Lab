/* QEMU virt 16550A UART, base 0x10000000. */
unsigned long uart_base_addr = 0x10000000UL;

#define UART_RBR_OFF 0x0
#define UART_THR_OFF 0x0
#define UART_IER_OFF 0x1
#define UART_IIR_OFF 0x2
#define UART_LCR_OFF 0x3
#define UART_MCR_OFF 0x4
#define UART_LSR_OFF 0x5

#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#define UART_REG8(off) (*(volatile unsigned char *)(uart_base_addr + (off)))

void uart_set_base(unsigned long base) {
    uart_base_addr = base;
}

unsigned char uart_read_reg(int off) {
    return UART_REG8(off);
}

void uart_write_reg(int off, unsigned char val) {
    UART_REG8(off) = val;
}

char uart_getc(void) {
    while ((UART_REG8(UART_LSR_OFF) & LSR_DR) == 0)
        ;
    char c = (char)UART_REG8(UART_RBR_OFF);
    return c == '\r' ? '\n' : c;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((UART_REG8(UART_LSR_OFF) & LSR_TDRQ) == 0)
        ;
    UART_REG8(UART_THR_OFF) = (unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int c = 60; c >= 0; c -= 4) {
        unsigned long n = (h >> c) & 0xf;
        uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

void uart_putb(unsigned char c) {
    while ((UART_REG8(UART_LSR_OFF) & LSR_TDRQ) == 0)
        ;
    UART_REG8(UART_THR_OFF) = c;
}

unsigned char uart_getb(void) {
    while ((UART_REG8(UART_LSR_OFF) & LSR_DR) == 0)
        ;
    return UART_REG8(UART_RBR_OFF);
}
