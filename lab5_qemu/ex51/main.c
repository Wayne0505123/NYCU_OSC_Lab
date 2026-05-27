extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);
extern void* kmalloc(unsigned long size);
extern void* alloc_page();

#define STACK_SIZE 0x1000

struct task_struct {
    struct thread_struct {
        unsigned long ra;
        unsigned long sp;
        unsigned long s[12];
    } thread;
    int pid;
    unsigned long kernel_sp;
    unsigned long user_sp;
    unsigned long stack;
    int state;
    struct task_struct* next;
};

enum {
    TASK_RUNNING = 0,
    TASK_ZOMBIE = 1,
};

static int nr_threads = 0;
static struct task_struct* run_queue = 0;

static void enqueue(struct task_struct** queue, struct task_struct* task) {
    if (*queue == 0) {
        *queue = task;
        task->next = task;
    } else {
        struct task_struct* tail = (*queue)->next;
        (*queue)->next = task;
        task->next = tail;
    }
}

struct task_struct* get_current() {
    register struct task_struct* current asm("tp");
    return current;
}

extern void switch_to(struct task_struct* prev, struct task_struct* next);

static void remove_task(struct task_struct* task) {
    if (run_queue == 0 || task == 0)
        return;

    struct task_struct* prev = run_queue;
    while (prev->next != task && prev->next != run_queue)
        prev = prev->next;

    if (prev->next != task)
        return;

    if (task->next == task) {
        run_queue = 0;
    } else {
        prev->next = task->next;
        if (run_queue == task)
            run_queue = task->next;
    }
}

void kill_zombies() {
    if (run_queue == 0)
        return;

    struct task_struct* start = run_queue;
    struct task_struct* cur = start;

    do {
        struct task_struct* next = cur->next;
        if (cur->state == TASK_ZOMBIE)
            remove_task(cur);
        cur = next;
    } while (run_queue && cur != start);
}

void schedule() {
    struct task_struct* prev = get_current();

    if (run_queue == 0)
        return;

    struct task_struct* next = run_queue;
    do {
        if (next != prev && next->state != TASK_ZOMBIE)
            break;
        next = next->next;
    } while (next != run_queue);

    if (next == 0 || next == prev)
        return;

    run_queue = next->next;

    switch_to(prev, next);
}

void idle() {
    while (1) {
        kill_zombies();
        schedule();
    }
}

void thread_exit() {
    get_current()->state = TASK_ZOMBIE;
    schedule();
    while (1)
        ;
}

void foo() {
    for (int i = 0; i < 5; i++) {
        uart_puts("Process ID: ");
        uart_hex(get_current()->pid);
        uart_puts(" ");
        uart_hex(i);
        uart_puts("\n");
        for (int i = 0; i < 100000000; i++)
            ;
        schedule();
    }
    thread_exit();
}

struct task_struct* kthread_create(void (*threadfn)()) {
    struct task_struct* task = kmalloc(sizeof(struct task_struct));
    task->pid = nr_threads++;
    task->stack = (unsigned long)alloc_page();
    task->thread.ra = (unsigned long)threadfn;
    task->thread.sp = task->stack + STACK_SIZE;
    task->state = TASK_RUNNING;
    enqueue(&run_queue, task);
    return task;
}

void start_kernel() {
    uart_puts("\nStarting kernel ...\n");
    /* Initialize the thread pointer */
    asm volatile("move tp, %0" : : "r"(kthread_create(idle)));
    for (int i = 0; i < 3; i++)
        kthread_create(foo);
    idle();
}

void do_trap() {
    uart_puts("Kernel panic - do_trap\n");
    while (1)
        ;
}
