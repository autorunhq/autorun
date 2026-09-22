.text
.balign 16

.macro save_host
    adrp x16, saved_host
    add x16, x16, :lo12:saved_host
    mov x17, sp
    stp x17, x18, [x16]
    stp x19, x20, [x16, #16]
    stp x21, x22, [x16, #32]
    stp x23, x24, [x16, #48]
    stp x25, x26, [x16, #64]
    stp x27, x28, [x16, #80]
    stp x29, x30, [x16, #96]
    adrp x16, saved_host_vec
    add x16, x16, :lo12:saved_host_vec
    stp q8, q9, [x16]
    stp q10, q11, [x16, #32]
    stp q12, q13, [x16, #64]
    stp q14, q15, [x16, #96]
.endm

.macro load_inputs
    adrp x16, input_gpr
    add x16, x16, :lo12:input_gpr
    ldr x8, [x16]
    ldr x0, [x16, #8]
    ldr x1, [x16, #16]
    ldr x27, [x16, #24]
    ldr x29, [x16, #32]
    ldr x25, [x16, #40]
    ldr x26, [x16, #48]
    ldr x2, [x16, #56]
    ldr x3, [x16, #64]
    ldr x4, [x16, #72]
    ldr x5, [x16, #80]
    ldr x19, [x16, #88]
    ldr x20, [x16, #96]
    ldr x21, [x16, #104]
    ldr x22, [x16, #112]
    adrp x17, input_vec
    add x17, x17, :lo12:input_vec
    ldp q0, q1, [x17]
    ldp q2, q3, [x17, #32]
    ldp q4, q5, [x17, #64]
    ldp q6, q7, [x17, #96]
    ldp q8, q9, [x17, #128]
    ldp q10, q11, [x17, #160]
    ldp q12, q13, [x17, #192]
    ldp q14, q15, [x17, #224]
.endm

.global invoke_dispatch
.type invoke_dispatch, %function
invoke_dispatch:
    save_host
    adrp x18, fake_teb
    add x18, x18, :lo12:fake_teb
    load_inputs
    adrp x16, initial_target
    ldr x9, [x16, :lo12:initial_target]
    adrp x16, DispatchJump
    add x16, x16, :lo12:DispatchJump
    blr x16
    brk #1

.global invoke_ret
.type invoke_ret, %function
invoke_ret:
    save_host
    adrp x18, fake_teb
    add x18, x18, :lo12:fake_teb
    load_inputs
    adrp x16, initial_target
    ldr x30, [x16, :lo12:initial_target]
    b RetToEntryThunk

.global invoke_begin
.type invoke_begin, %function
invoke_begin:
    save_host
    adrp x18, fake_teb
    add x18, x18, :lo12:fake_teb
    adrp x16, BeginSimulation
    add x16, x16, :lo12:BeginSimulation
    blr x16
    brk #1

.balign 16
.word 0xd63f0200
.global direct_target
.type direct_target, %function
direct_target:
    adrp x16, observed_gpr
    add x16, x16, :lo12:observed_gpr
    str x8, [x16]
    str x0, [x16, #8]
    str x1, [x16, #16]
    str x27, [x16, #24]
    str x29, [x16, #32]
    str x25, [x16, #40]
    str x26, [x16, #48]
    str x2, [x16, #56]
    str x3, [x16, #64]
    str x4, [x16, #72]
    str x5, [x16, #80]
    str x19, [x16, #88]
    str x20, [x16, #96]
    str x21, [x16, #104]
    str x22, [x16, #112]
    mov x17, sp
    str x17, [x16, #120]
    str x23, [x16, #128]
    adrp x16, observed_vec
    add x16, x16, :lo12:observed_vec
    stp q0, q1, [x16]
    stp q2, q3, [x16, #32]
    stp q4, q5, [x16, #64]
    stp q6, q7, [x16, #96]
    stp q8, q9, [x16, #128]
    stp q10, q11, [x16, #160]
    stp q12, q13, [x16, #192]
    stp q14, q15, [x16, #224]
    mov w17, #1
    adrp x16, observed_kind
    str w17, [x16, :lo12:observed_kind]
    b test_return

.balign 16
.word entry_target - guest_target
.global guest_target
guest_target:
    nop
.global entry_target
.type entry_target, %function
entry_target:
    adrp x16, observed_entry
    add x16, x16, :lo12:observed_entry
    mov x17, sp
    stp x17, x30, [x16]
    str x4, [x16, #16]
    str x23, [x16, #24]
    mov w17, #2
    adrp x16, observed_kind
    str w17, [x16, :lo12:observed_kind]
    b test_return

.global ret_sentinel
ret_sentinel:
    ret

.type test_return, %function
test_return:
    adrp x16, saved_host_vec
    add x16, x16, :lo12:saved_host_vec
    ldp q8, q9, [x16]
    ldp q10, q11, [x16, #32]
    ldp q12, q13, [x16, #64]
    ldp q14, q15, [x16, #96]
    adrp x16, saved_host
    add x16, x16, :lo12:saved_host
    ldp x17, x18, [x16]
    mov sp, x17
    ldp x19, x20, [x16, #16]
    ldp x21, x22, [x16, #32]
    ldp x23, x24, [x16, #48]
    ldp x25, x26, [x16, #64]
    ldp x27, x28, [x16, #80]
    ldp x29, x30, [x16, #96]
    ret

.section .note.GNU-stack,"",%progbits
