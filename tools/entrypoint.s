; a x86_64 asm for _start which get cmdline args and invokes main
; with int main(int argc,char* argv[])

section .text
    global _start

    extern main

_start:
    ; Setup stack frame (optional, but good practice)
    push rbp
    mov rbp, rsp

    ; argc is already on the stack (passed by the kernel)
    ; argv is also already on the stack (passed by the kernel)

    ; Get argc (argument count) from the stack
    mov rdi, [rsp]  ; argc is at the top of the stack

    ; Get argv (argument vector) from the stack
    lea rsi, [rsp+8] ; argv starts 8 bytes after argc (assuming 64-bit pointers)

    ; Call main(argc, argv)
    call main

    ; Get the return value of main
    mov rdi, rax    ; main's return value is in rax, move it to rdi for exit

    ; Call exit(return_code)
    mov rax, 60     ; syscall number for exit is 60
    syscall         ; invoke the syscall

    ; If exit fails, halt
    hlt
