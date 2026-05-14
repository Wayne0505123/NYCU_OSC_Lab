extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);

#define UART_BASE 0x10000000UL
#define UART_RBR  (volatile unsigned char*)(UART_BASE + 0x0)
#define UART_THR  (volatile unsigned char*)(UART_BASE + 0x0)
#define UART_IER  (volatile unsigned char*)(UART_BASE + 0x1)
#define UART_IIR  (volatile unsigned char*)(UART_BASE + 0x2)
#define UART_MCR  (volatile unsigned char*)(UART_BASE + 0x4)
#define UART_LSR  (volatile unsigned char*)(UART_BASE + 0x5)

#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#define UART_IRQ  0x0a

#define PLIC_BASE            0x0c000000UL
#define PLIC_PRIORITY(irq)   (PLIC_BASE + (irq) * 4)
#define PLIC_ENABLE(hart)    (PLIC_BASE + 0x002080 + (hart) * 0x0100)
#define PLIC_THRESHOLD(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)
#define PLIC_CLAIM(hart)     (PLIC_BASE + 0x201004 + (hart) * 0x2000)

unsigned long boot_cpu_hartid = 0;

static inline void write32(unsigned long addr, unsigned int value) {
    *(volatile unsigned int*)addr = value;
}

static inline unsigned int read32(unsigned long addr) {
    return *(volatile unsigned int*)addr;
}

void uart_init() {
    /*
     * UART IER bit 0:
     * Enable Received Data Available interrupt.
     */
    *UART_IER |= 0x01;

    /*
     * UART MCR bit 3 OUT2:
     * Route UART interrupt output to interrupt controller.
     */
    *UART_MCR |= 0x08;
}

void irq_enable() {
    /*
     * sstatus.SIE = 1
     * Enable global supervisor interrupt.
     */
    asm volatile("csrsi sstatus, 2");
}

void enable_external_interrupt() {
    /*
     * sie.SEIE = 1
     * Enable supervisor external interrupt.
     */
    asm volatile(
        "li t0, (1 << 9);"
        "csrs sie, t0;"
        ::: "t0", "memory");
}

void plic_init() {
    /*
     * 1. Set UART IRQ priority > 0.
     */
    write32(PLIC_PRIORITY(UART_IRQ), 1);

    /*
     * 2. Enable UART IRQ for boot hart.
     * UART_IRQ = 10, so set bit 10.
     */
    unsigned int enable = read32(PLIC_ENABLE(boot_cpu_hartid));
    enable |= (1U << UART_IRQ);
    write32(PLIC_ENABLE(boot_cpu_hartid), enable);

    /*
     * 3. Threshold = 0.
     * Accept all interrupts with priority > 0.
     */
    write32(PLIC_THRESHOLD(boot_cpu_hartid), 0);

    /*
     * 4. Enable supervisor external interrupt in sie.
     */
    enable_external_interrupt();
}

int plic_claim() {
    /*
     * Read claim register.
     * PLIC returns the highest-priority pending IRQ number.
     */
    return (int)read32(PLIC_CLAIM(boot_cpu_hartid));
}

void plic_complete(int irq) {
    /*
     * Write IRQ number back to claim/complete register.
     * This unlocks the gateway so future UART interrupts can arrive.
     */
    write32(PLIC_CLAIM(boot_cpu_hartid), (unsigned int)irq);
}

void do_trap() {
    int irq = plic_claim();

    if (irq == UART_IRQ) {
        /*
         * UART RX interrupt.
         * Read RBR to consume the character and clear UART pending state.
         */
        char c = (char)*UART_RBR;
        uart_putc(c == '\r' ? '\n' : c);
    }

    if (irq)
        plic_complete(irq);
}

void start_kernel() {
    uart_puts("\nStarting kernel ...\n");

    plic_init();
    uart_init();
    irq_enable();

    while (1)
        ;
}
