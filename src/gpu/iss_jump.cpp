// Win64 non-unwinding recovery used by the ISS rasterizer when speculative
// reads fault. Keep the register layout in sync with the upstream compat_win ABI.
extern "C" __attribute__((returns_twice)) int tlg_setjmp(unsigned long long buffer[32]);
extern "C" __attribute__((noreturn)) void tlg_longjmp(unsigned long long buffer[32], int value);

__asm__(
    ".text\n"
    ".globl tlg_setjmp\n"
    ".def tlg_setjmp; .scl 2; .type 32; .endef\n"
    "tlg_setjmp:\n"
    "    mov [rcx],rbx\n    mov [rcx+8],rbp\n    mov [rcx+16],rdi\n    mov [rcx+24],rsi\n"
    "    mov [rcx+32],r12\n    mov [rcx+40],r13\n    mov [rcx+48],r14\n    mov [rcx+56],r15\n"
    "    lea rdx,[rsp+8]\n    mov [rcx+64],rdx\n    mov rdx,[rsp]\n    mov [rcx+72],rdx\n"
    "    movups [rcx+80],xmm6\n    movups [rcx+96],xmm7\n    movups [rcx+112],xmm8\n"
    "    movups [rcx+128],xmm9\n    movups [rcx+144],xmm10\n    movups [rcx+160],xmm11\n"
    "    movups [rcx+176],xmm12\n    movups [rcx+192],xmm13\n    movups [rcx+208],xmm14\n"
    "    movups [rcx+224],xmm15\n    stmxcsr [rcx+240]\n    fnstcw [rcx+244]\n"
    "    xor eax,eax\n    ret\n"
    ".globl tlg_longjmp\n"
    ".def tlg_longjmp; .scl 2; .type 32; .endef\n"
    "tlg_longjmp:\n"
    "    mov eax,edx\n    test eax,eax\n    jnz 1f\n    mov eax,1\n"
    "1:  mov rbx,[rcx]\n    mov rbp,[rcx+8]\n    mov rdi,[rcx+16]\n    mov rsi,[rcx+24]\n"
    "    mov r12,[rcx+32]\n    mov r13,[rcx+40]\n    mov r14,[rcx+48]\n    mov r15,[rcx+56]\n"
    "    movups xmm6,[rcx+80]\n    movups xmm7,[rcx+96]\n    movups xmm8,[rcx+112]\n"
    "    movups xmm9,[rcx+128]\n    movups xmm10,[rcx+144]\n    movups xmm11,[rcx+160]\n"
    "    movups xmm12,[rcx+176]\n    movups xmm13,[rcx+192]\n    movups xmm14,[rcx+208]\n"
    "    movups xmm15,[rcx+224]\n    ldmxcsr [rcx+240]\n    fldcw [rcx+244]\n"
    "    mov rsp,[rcx+64]\n    jmp qword ptr [rcx+72]\n");

#if defined(LBP3_HAS_ISS_GPU)
namespace {
using JumpBuffer = unsigned long long[32];
}
extern "C" int tlg_jump_bridge_self_test() {
    JumpBuffer buffer{};
    const int result = tlg_setjmp(buffer);
    if (result == 0) tlg_longjmp(buffer, 37);
    return result;
}
#endif
