extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);

extern int hextoi(const char* s, int n);
extern int align(int n, int byte);
extern int memcmp(const void* s1, const void* s2, int n);
extern void* alloc_page();

/*
 * QEMU virt + -initrd initramfs.cpio
 *
 * 在這份 exercise template 裡，initrd 預期被放在 0xa0200000。
 * 如果你跑出 Failed to exec user program，第一個要檢查的就是這個位址。
 */
#define INITRD_BASE 0x88200000UL
#define STACK_SIZE  0x1000

#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)

struct cpio_t {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

/*
 * 這個 struct layout 必須完全對應 start.S 的 save_context 順序：
 *
 * sd ra,  8 *  0(sp)
 * sd s0(user sp), 8 * 1(sp)
 * sd gp,  8 *  2(sp)
 * ...
 * sd sepc,    8 * 31(sp)
 * sd sstatus, 8 * 32(sp)
 * sd scause,  8 * 33(sp)
 * sd stval,   8 * 34(sp)
 */
struct pt_regs {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;
    unsigned long t0;
    unsigned long t1;
    unsigned long t2;
    unsigned long s0;
    unsigned long s1;
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;
    unsigned long a4;
    unsigned long a5;
    unsigned long a6;
    unsigned long a7;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
    unsigned long t3;
    unsigned long t4;
    unsigned long t5;
    unsigned long t6;
    unsigned long sepc;
    unsigned long sstatus;
    unsigned long scause;
    unsigned long stval;
};

static int cpio_name_match(const char* entry_name,
                           const char* target,
                           int namesize) {
    /*
     * CPIO newc 的 namesize 包含結尾 '\0'。
     * 例如 "prog.bin" 的 namesize 通常是 9。
     *
     * 這裡要比較 namesize bytes，才能跟 archive entry 完整匹配。
     */
    return memcmp(entry_name, target, namesize) == 0;
}

int exec(const char* filename) {
    char* p = (char*)INITRD_BASE;

    while (memcmp(p + sizeof(struct cpio_t), "TRAILER!!!", 10)) {
        struct cpio_t* hdr = (struct cpio_t*)p;

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);

        int headsize = align(sizeof(struct cpio_t) + namesize, 4);
        int datasize = align(filesize, 4);

        char* name = p + sizeof(struct cpio_t);
        char* data = p + headsize;

        if (cpio_name_match(name, filename, namesize)) {
            unsigned long user_entry = (unsigned long)data;
            unsigned long user_stack = (unsigned long)alloc_page() + STACK_SIZE;

            /*
             * 保存目前 kernel stack。
             *
             * start.S 的 trap entry 使用：
             *     csrrw sp, sscratch, sp
             *
             * 當 user trap 進 kernel 時：
             *     sp       = kernel sp
             *     sscratch = user sp
             */
            unsigned long kernel_sp;
            asm volatile("mv %0, sp" : "=r"(kernel_sp));

            /*
             * 設定 sstatus：
             *
             * SPP = 0：sret 後回到 U-mode
             * SPIE = 1：sret 後讓 SIE 恢復為 enabled 狀態
             */
            unsigned long sstatus;
            asm volatile("csrr %0, sstatus" : "=r"(sstatus));

            sstatus &= ~SSTATUS_SPP;
            sstatus |= SSTATUS_SPIE;

            /*
             * sepc    = user program entry
             * sstatus = return privilege 設定
             * sscratch = kernel stack pointer
             * sp      = user stack pointer
             * sret    = 從 S-mode 切到 U-mode
             */
            asm volatile(
                "csrw sepc, %0\n"
                "csrw sstatus, %1\n"
                "csrw sscratch, %2\n"
                "mv sp, %3\n"
                "sret\n"
                :
                : "r"(user_entry),
                  "r"(sstatus),
                  "r"(kernel_sp),
                  "r"(user_stack)
                : "memory"
            );

            return 0;
        }

        p += headsize + datasize;
    }

    return -1;
}

void do_trap(struct pt_regs* regs) {
    uart_puts("sepc: ");
    uart_hex(regs->sepc);
    uart_puts(", scause: ");
    uart_hex(regs->scause);
    uart_puts("\n");

    if (regs->scause == 8) {
        regs->sepc += 4;
    } else {
        uart_puts("Unexpected trap, stop.\n");
        while (1) {}
    }
}

void start_kernel() {
    uart_puts("\nStarting kernel ...\n");

    if (exec("prog.bin"))
        uart_puts("Failed to exec user program!\n");

    while (1) {
        uart_putc(uart_getc());
    }
}
