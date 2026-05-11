#define UART_BASE 0x10000000UL
#define UART_RBR  (unsigned char*)(UART_BASE + 0x0)
#define UART_THR  (unsigned char*)(UART_BASE + 0x0)
#define UART_LSR  (unsigned char*)(UART_BASE + 0x5)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

char uart_getc() {
    while (((*UART_LSR) & LSR_DR) == 0) ;
    return *UART_RBR;
}

void uart_putc(char c) {
    while (((*UART_LSR) & LSR_TDRQ) == 0) ;

    if (c == '\r') {
        *UART_THR = '\r';
        while (((*UART_LSR) & LSR_TDRQ) == 0) ;
        *UART_THR = '\n';
        return;
    }

    if (c == '\n') {
        *UART_THR = '\r';
        while (((*UART_LSR) & LSR_TDRQ) == 0) ;
    }

    *UART_THR = c;
}

void uart_puts(const char* s) {
    while (*s) {
        uart_putc(*s);
        s++;
    }
}

void start_kernel() {
    uart_puts("\nStarting kernel ...\n");
    while (1) {
        uart_putc(uart_getc());
    }
}
