; x86_64 Linux ELF assembly for _start
; Sets up argc and argv for main function call
; Then exits with main's return value

section .text
    global _start
    extern main

_start:
    ; Get stack pointer
    mov rdi, [rsp]      ; argc (first parameter to main)
    lea rsi, [rsp+8]    ; argv (second parameter to main)
    
    ; Align stack to 16 bytes (required by System V ABI)
    ; We need to ensure stack is 16-byte aligned before call
    and rsp, -16
    
    ; Call main function
    call main
    
    ; main's return value is in rax, use it as exit code
    mov rdi, rax        ; exit code
    mov rax, 60         ; syscall number for exit
    syscall
