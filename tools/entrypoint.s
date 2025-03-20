; a x86_64 asm for _start which invokes main
; with int main(int argc,char* argv[])

section .text
    global _start

    extern main

_start:
    ; Setup stack frame (optional, but good practice)
    push rbp
    mov rbp, rsp

    ; Get argc (argument count) from the stack
    pop rdi         ; argc is the first argument

    ; Get argv (argument vector) from the stack
    mov rsi, rsp    ; argv is the second argument

    ; Call main(argc, argv)
    call main

    ; Get the return value of main
    mov rdi, rax    ; main's return value is in rax, move it to rdi for exit

    ; Call exit(return_code)
    mov rax, 60     ; syscall number for exit is 60
    syscall         ; invoke the syscall

    ; If exit fails, halt
    hlt
