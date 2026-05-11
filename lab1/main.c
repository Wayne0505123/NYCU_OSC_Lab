extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);

#define SBI_EXT_SET_TIMER 0x0
#define SBI_EXT_SHUTDOWN  0x8
#define SBI_EXT_BASE      0x10

enum sbi_ext_base_fid {
    SBI_EXT_BASE_GET_SPEC_VERSION,
    SBI_EXT_BASE_GET_IMP_ID,
    SBI_EXT_BASE_GET_IMP_VERSION,
    SBI_EXT_BASE_PROBE_EXT,
    SBI_EXT_BASE_GET_MVENDORID,
    SBI_EXT_BASE_GET_MARCHID,
    SBI_EXT_BASE_GET_MIMPID,
};

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(int ext,
                        int fid,
                        unsigned long arg0,
                        unsigned long arg1,
                        unsigned long arg2,
                        unsigned long arg3,
                        unsigned long arg4,
                        unsigned long arg5) {
    struct sbiret ret;
    register unsigned long a0 asm("a0") = (unsigned long)arg0;
    register unsigned long a1 asm("a1") = (unsigned long)arg1;
    register unsigned long a2 asm("a2") = (unsigned long)arg2;
    register unsigned long a3 asm("a3") = (unsigned long)arg3;
    register unsigned long a4 asm("a4") = (unsigned long)arg4;
    register unsigned long a5 asm("a5") = (unsigned long)arg5;
    register unsigned long a6 asm("a6") = (unsigned long)fid;
    register unsigned long a7 asm("a7") = (unsigned long)ext;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    ret.error = a0;
    ret.value = a1;
    return ret;
}

long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_SPEC_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_ID,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_GET_IMP_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

long sbi_probe_extension(int extid) {
    return sbi_ecall(SBI_EXT_BASE,
                     SBI_EXT_BASE_PROBE_EXT,
                     extid, 0, 0, 0, 0, 0).value;
}

static int strcmp_simple(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void print_prompt(void) {
    uart_puts("opi-rv2> ");
}

static void shell_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("  help  - show all commands.\n");
    uart_puts("  hello - print Hello world.\n");
    uart_puts("  info  - print system info.\n");
}

static void shell_hello(void) {
    uart_puts("Hello world.\n");
}

static void shell_info(void) {
    uart_puts("System information:\n");
    uart_puts("    OpenSBI specification version: ");
    uart_hex(sbi_get_spec_version());
    uart_puts("\n");

    uart_puts("    implementation ID: ");
    uart_hex(sbi_get_impl_id());
    uart_puts("\n");

    uart_puts("    implementation version: ");
    uart_hex(sbi_get_impl_version());
    uart_puts("\n");
}

static void shell_execute(const char* cmd) {
    if (strcmp_simple(cmd, "help")) {
        shell_help();
    } else if (strcmp_simple(cmd, "hello")) {
        shell_hello();
    } else if (strcmp_simple(cmd, "info")) {
        shell_info();
    } else if (cmd[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}

void start_kernel() {
    char buf[128];
    int idx = 0;

    uart_puts("\nStarting kernel ...\n\n");
    print_prompt();

    while (1) {
        char c = uart_getc();

        if (c == '\n') {
            uart_putc('\n');
            buf[idx] = '\0';
            shell_execute(buf);
            idx = 0;
            print_prompt();
        } else if (c == 8 || c == 127) {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
        } else {
            if (idx < (int)sizeof(buf) - 1) {
                buf[idx++] = c;
                uart_putc(c);
            }
        }
    }
}
