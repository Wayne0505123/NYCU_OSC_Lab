unsigned long uart_base_addr = 0xD4017000UL;
int uart_reg_shift = 2;
int uart_reg_io_width = 4;

#define UART_RBR 0
#define UART_THR 0
#define UART_LSR 5

#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#define UART_REG(off) (uart_base_addr + ((off) << uart_reg_shift))

void uart_set_base(unsigned long base) {
    uart_base_addr = base;
}

void uart_set_config(int reg_shift, int reg_io_width) {
    uart_reg_shift = reg_shift;
    uart_reg_io_width = reg_io_width;
}

static unsigned int uart_read_reg(int off) {
    if (uart_reg_io_width == 4)
        return *(volatile unsigned int *)UART_REG(off);

    return *(volatile unsigned char *)UART_REG(off);
}

static void uart_write_reg(int off, unsigned int val) {
    if (uart_reg_io_width == 4)
        *(volatile unsigned int *)UART_REG(off) = val;
    else
        *(volatile unsigned char *)UART_REG(off) = val;
}

char uart_getc(void) {
    while ((uart_read_reg(UART_LSR) & LSR_DR) == 0)
        ;

    char c = (char)(uart_read_reg(UART_RBR) & 0xff);
    return c == '\r' ? '\n' : c;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((uart_read_reg(UART_LSR) & LSR_TDRQ) == 0)
        ;

    uart_write_reg(UART_THR, (unsigned int)c);
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

void uart_put_uint(unsigned int x) {
    char buf[16];
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

void uart_putb(unsigned char c) {
    while ((uart_read_reg(UART_LSR) & LSR_TDRQ) == 0)
        ;

    uart_write_reg(UART_THR, (unsigned int)c);
}

unsigned char uart_getb(void) {
    while ((uart_read_reg(UART_LSR) & LSR_DR) == 0)
        ;

    return (unsigned char)(uart_read_reg(UART_RBR) & 0xff);
}
