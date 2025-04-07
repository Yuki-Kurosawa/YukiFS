# x86_64 Linux ELF assembly for _start
# Sets up argc and argv for main function call
# Then exits with main's return value

.section .text
    .globl _start
    .extern main

_start:
    # Get stack pointer
    movq (%rsp), %rdi      # argc (first parameter to main)
    leaq 8(%rsp), %rsi    # argv (second parameter to main)

    # Align stack to 16 bytes (required by System V ABI)
    # We need to ensure stack is 16-byte aligned before call
    andq $-16, %rsp

    # Call main function
    call main

    # main's return value is in rax, use it as exit code
    movq %rax, %rdi        # exit code
    movq $60, %rax        # syscall number for exit
    syscall
