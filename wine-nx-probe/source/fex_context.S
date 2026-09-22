.text
.balign 4
.global wine_nx_fex_continue_context
.type wine_nx_fex_continue_context, %function
wine_nx_fex_continue_context:
    brk #0xfec0
    b .
.size wine_nx_fex_continue_context, . - wine_nx_fex_continue_context

.global __wrap___libnx_exception_entry
.type __wrap___libnx_exception_entry, %function
__wrap___libnx_exception_entry:
    cbz x1, .Loriginal
    ldr x2, [x1, #0x58]
    adrp x3, wine_nx_fex_continue_context
    add x3, x3, :lo12:wine_nx_fex_continue_context
    cmp x2, x3
    b.eq .Lrestore

    mrs x2, tpidrro_el0
    adrp x3, wine_nx_fex_exception_slots
    add x3, x3, :lo12:wine_nx_fex_exception_slots
    mov w4, #256
.Lfind:
    ldar x5, [x3]
    cmp x2, x5
    b.eq .Lcapture
    add x3, x3, #24
    subs w4, w4, #1
    b.ne .Lfind
.Loriginal:
    b __real___libnx_exception_entry

.Lcapture:
    ldr x4, [x3, #16]
    ldr x3, [x3, #8]
    ldr x5, [x1, #80]
    cmp x5, x3
    b.lo .Lsave
    cmp x5, x4
    b.hs .Lsave
    sub x5, x5, #0x400
    and x5, x5, #-16
    add x6, x3, #0x1000
    cmp x5, x6
    b.lo .Loriginal
    mov x3, x5
    mov x4, x5
.Lsave:
    str w0, [x3]
    mrs x2, fpcr
    str w2, [x3, #4]
    mrs x2, fpsr
    str w2, [x3, #8]
    mov w2, #0xfec0
    str w2, [x3, #12]
    ldp x5, x6, [x1, #0]
    stp x5, x6, [x3, #16]
    ldp x5, x6, [x1, #16]
    stp x5, x6, [x3, #32]
    ldp x5, x6, [x1, #32]
    stp x5, x6, [x3, #48]
    ldp x5, x6, [x1, #48]
    stp x5, x6, [x3, #64]
    ldr x5, [x1, #64]
    str x5, [x3, #80]
    stp x9, x10, [x3, #88]
    stp x11, x12, [x3, #104]
    stp x13, x14, [x3, #120]
    stp x15, x16, [x3, #136]
    stp x17, x18, [x3, #152]
    stp x19, x20, [x3, #168]
    stp x21, x22, [x3, #184]
    stp x23, x24, [x3, #200]
    stp x25, x26, [x3, #216]
    stp x27, x28, [x3, #232]
    str x29, [x3, #248]
    ldr x5, [x1, #72]
    str x5, [x3, #256]
    ldr x5, [x1, #80]
    str x5, [x3, #264]
    ldr x5, [x1, #88]
    str x5, [x3, #272]
    stp q0, q1, [x3, #288]
    stp q2, q3, [x3, #320]
    stp q4, q5, [x3, #352]
    stp q6, q7, [x3, #384]
    stp q8, q9, [x3, #416]
    stp q10, q11, [x3, #448]
    stp q12, q13, [x3, #480]
    stp q14, q15, [x3, #512]
    stp q16, q17, [x3, #544]
    stp q18, q19, [x3, #576]
    stp q20, q21, [x3, #608]
    stp q22, q23, [x3, #640]
    stp q24, q25, [x3, #672]
    stp q26, q27, [x3, #704]
    stp q28, q29, [x3, #736]
    stp q30, q31, [x3, #768]
    ldr w5, [x1, #96]
    str w5, [x3, #800]
    ldr w5, [x1, #100]
    str w5, [x3, #804]
    ldr w5, [x1, #104]
    str w5, [x3, #808]
    ldr w5, [x1, #108]
    str w5, [x3, #812]
    ldr x5, [x1, #112]
    str x5, [x3, #816]
    str x3, [x1]
    str x4, [x1, #80]
    adrp x5, __libnx_exception_returnentry
    add x5, x5, :lo12:__libnx_exception_returnentry
    str x5, [x1, #88]
    mov w0, wzr
    svc #0x28
    b .

.Lrestore:
    ldr x2, [x1]
    ldp x3, x4, [x2, #8]
    stp x3, x4, [x1, #0]
    ldp x3, x4, [x2, #24]
    stp x3, x4, [x1, #16]
    ldp x3, x4, [x2, #40]
    stp x3, x4, [x1, #32]
    ldp x3, x4, [x2, #56]
    stp x3, x4, [x1, #48]
    ldr x3, [x2, #72]
    str x3, [x1, #64]
    ldr x3, [x2, #248]
    str x3, [x1, #72]
    ldr x3, [x2, #256]
    str x3, [x1, #80]
    ldr x3, [x2, #264]
    str x3, [x1, #88]
    ldr w3, [x2, #4]
    str w3, [x1, #96]
    ldr w3, [x2, #784]
    msr fpcr, x3
    ldr w3, [x2, #788]
    msr fpsr, x3
    ldp q0, q1, [x2, #272]
    ldp q2, q3, [x2, #304]
    ldp q4, q5, [x2, #336]
    ldp q6, q7, [x2, #368]
    ldp q8, q9, [x2, #400]
    ldp q10, q11, [x2, #432]
    ldp q12, q13, [x2, #464]
    ldp q14, q15, [x2, #496]
    ldp q16, q17, [x2, #528]
    ldp q18, q19, [x2, #560]
    ldp q20, q21, [x2, #592]
    ldp q22, q23, [x2, #624]
    ldp q24, q25, [x2, #656]
    ldp q26, q27, [x2, #688]
    ldp q28, q29, [x2, #720]
    ldp q30, q31, [x2, #752]
    ldp x9, x10, [x2, #80]
    ldp x11, x12, [x2, #96]
    ldp x13, x14, [x2, #112]
    ldp x15, x16, [x2, #128]
    ldp x17, x18, [x2, #144]
    ldp x19, x20, [x2, #160]
    ldp x21, x22, [x2, #176]
    ldp x23, x24, [x2, #192]
    ldp x25, x26, [x2, #208]
    ldp x27, x28, [x2, #224]
    ldr x29, [x2, #240]
    mov w0, wzr
    svc #0x28
    b .
.size __wrap___libnx_exception_entry, . - __wrap___libnx_exception_entry
.section .note.GNU-stack,"",%progbits
