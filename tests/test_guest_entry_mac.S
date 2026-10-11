.text
.globl _entry_body
.globl _entry_body_end
_entry_body:
    pushq %rbp
    movq %rsp,%rbp
    movq %rax,0(%rdi)
    movq %rbx,8(%rdi)
    movq %rcx,16(%rdi)
    movq %rdx,24(%rdi)
    movq %rsi,32(%rdi)
    movq %rdi,40(%rdi)
    movq %r8,48(%rdi)
    movq %r9,56(%rdi)
    movq %r10,64(%rdi)
    movq %r11,72(%rdi)
    movq %r12,80(%rdi)
    movq %r13,88(%rdi)
    movq %r14,96(%rdi)
    movq %r15,104(%rdi)
    movq %rbp,112(%rdi)
    movq %rsp,120(%rdi)
    pushfq
    popq 128(%rdi)
    movq (%rsp),%rax
    movq %rax,136(%rdi)
    vmovdqu %ymm0,256(%rdi)
    vmovdqu %ymm1,288(%rdi)
    vmovdqu %ymm2,320(%rdi)
    vmovdqu %ymm3,352(%rdi)
    vmovdqu %ymm4,384(%rdi)
    vmovdqu %ymm5,416(%rdi)
    vmovdqu %ymm6,448(%rdi)
    vmovdqu %ymm7,480(%rdi)
    vmovdqu %ymm8,512(%rdi)
    vmovdqu %ymm9,544(%rdi)
    vmovdqu %ymm10,576(%rdi)
    vmovdqu %ymm11,608(%rdi)
    vmovdqu %ymm12,640(%rdi)
    vmovdqu %ymm13,672(%rdi)
    vmovdqu %ymm14,704(%rdi)
    vmovdqu %ymm15,736(%rdi)
    movq 0(%rdi),%rax
    popq %rbp
    retq
_entry_body_end:
.globl _entry_call
.globl _entry_return
_entry_call:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8,%rsp
    movq %rsp,160(%rdi)
    movq $1,%rax
    movq $2,%rbx
    movq $3,%rcx
    movq $4,%rdx
    movq $5,%rsi
    movq $8,%r8
    movq $9,%r9
    movq $10,%r10
    movq $11,%r11
    movq $12,%r12
    movq $13,%r13
    movq $14,%r14
    movq $15,%r15
    movq $16,%rbp
    vpcmpeqd %ymm0,%ymm0,%ymm0
    vmovdqa %ymm0,%ymm1
    vmovdqa %ymm0,%ymm2
    vmovdqa %ymm0,%ymm3
    vmovdqa %ymm0,%ymm4
    vmovdqa %ymm0,%ymm5
    vmovdqa %ymm0,%ymm6
    vmovdqa %ymm0,%ymm7
    vmovdqa %ymm0,%ymm8
    vmovdqa %ymm0,%ymm9
    vmovdqa %ymm0,%ymm10
    vmovdqa %ymm0,%ymm11
    vmovdqa %ymm0,%ymm12
    vmovdqa %ymm0,%ymm13
    vmovdqa %ymm0,%ymm14
    vmovdqa %ymm0,%ymm15
    pushq $0x247
    popfq
    callq *_entry_code(%rip)
_entry_return:
    movq %rsp,168(%rdi)
    movq %rbp,176(%rdi)
    pushfq
    popq 184(%rdi)
    addq $8,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    retq
