unsigned long uart_base_addr = 0x10000000UL;

#define UART_RBR(base)  (unsigned char*)((base) + 0x0)
#define UART_THR(base)  (unsigned char*)((base) + 0x0)
#define UART_LSR(base)  (unsigned char*)((base) + 0x5)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

void uart_set_base(unsigned long base) {
    uart_base_addr = base;
}

char uart_getc() {
    while ((*UART_LSR(uart_base_addr) & LSR_DR) == 0)
        ;
    char c = (char)*UART_RBR(uart_base_addr);
    return c == '\r' ? '\n' : c;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((*UART_LSR(uart_base_addr) & LSR_TDRQ) == 0)
        ;
    *UART_THR(uart_base_addr) = c;
}

void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    unsigned long n;
    for (int c = 60; c >= 0; c -= 4) {
        n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc(n);
    }
}

void uart_putb(unsigned char c) {
    while ((*UART_LSR(uart_base_addr) & LSR_TDRQ) == 0)
        ;
    *UART_THR(uart_base_addr) = c;
}

unsigned char uart_getb(void) {
    while ((*UART_LSR(uart_base_addr) & LSR_DR) == 0)
        ;
    return *UART_RBR(uart_base_addr);
}
