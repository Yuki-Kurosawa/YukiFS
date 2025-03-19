typedef unsigned long long int uint64;

int write(int fd, const char *buf, int length)
{
    int ret;

    asm("mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%eax, %0"
        : "=r" (ret)
        : "r" ((uint64) 1), // #define SYS_write 1
          "r" ((uint64) fd),
          "r" ((uint64) buf),
          "r" ((uint64) length)
        : "%rax", "%rdi", "%rsi", "%rdx");

    return ret;
}


void _start(void)
{
    write(1, "\x54\x48\x49\x53\x20\x49\x53\x20\x42\x55\x49\x4c\x54\x20\x49\x4e\x20\x49\x4e\x46\x4f\x0a\x22\x61\x41\x41\x41\x22\x20\x27\x41\x41\x27\x0a",34); // Using sizeof directly

    // Make the exit syscall with code 0
    asm("mov %0, %%rax\n\t"
        "mov %1, %%rdi\n\t"
        "syscall"
        :: "i" (60), // SYS_exit is 60 on x86-64
           "i" (0)  // Exit code 0
        : "%rax", "%rdi");
    // No need for a separate exit function or main to return
}