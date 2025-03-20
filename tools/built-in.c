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


int main(int argc, char *argv[])
{
    write(1, BUILT_IN_DATA);
}