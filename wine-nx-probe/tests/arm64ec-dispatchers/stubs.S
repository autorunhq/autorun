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

.macro load_nonvolatile
    adrp x16, input_nonvolatile
    add x16, x16, :lo12:input_nonvolatile
    ldp x19, x20, [x16]
    ldp x21, x22, [x16, #16]
    ldp x23, x24, [x16, #32]
    ldp x25, x26, [x16, #48]
    ldp x27, x28, [x16, #64]
    ldr x29, [x16, #80]
    adrp x16, input_vec
    add x16, x16, :lo12:input_vec
    ldp q6, q7, [x16]
    ldp q8, q9, [x16, #32]
    ldp q10, q11, [x16, #64]
    ldp q12, q13, [x16, #96]
    ldp q14, q15, [x16, #128]
.endm

.global invoke_syscall_dispatcher
.type invoke_syscall_dispatcher, %function
invoke_syscall_dispatcher:
    save_host
    sub sp, sp, #128
    adrp x16, input_stack_args
    add x16, x16, :lo12:input_stack_args
    ldp x17, x18, [x16]
    stp x17, x18, [sp]
    ldp x17, x18, [x16, #16]
    stp x17, x18, [sp, #16]
    ldp x17, x18, [x16, #32]
    stp x17, x18, [sp, #32]
    ldp x17, x18, [x16, #48]
    stp x17, x18, [sp, #48]
    load_nonvolatile
    adrp x18, stale_teb
    add x18, x18, :lo12:stale_teb
    adrp x16, input_args
    add x16, x16, :lo12:input_args
    ldp x0, x1, [x16]
    ldp x2, x3, [x16, #16]
    ldp x4, x5, [x16, #32]
    ldp x6, x7, [x16, #48]
    adrp x16, input_syscall_id
    ldr w8, [x16, :lo12:input_syscall_id]
    adrp x9, syscall_return_target
    add x9, x9, :lo12:syscall_return_target
    adrp x16, __wine_syscall_dispatcher
    add x16, x16, :lo12:__wine_syscall_dispatcher
    blr x16
    brk #2

.global invoke_unix_dispatcher
.type invoke_unix_dispatcher, %function
invoke_unix_dispatcher:
    save_host
    load_nonvolatile
    adrp x18, stale_teb
    add x18, x18, :lo12:stale_teb
    adrp x16, unix_inputs
    add x16, x16, :lo12:unix_inputs
    ldp x0, x1, [x16]
    ldr x2, [x16, #16]
    adrp x16, __wine_unix_call_dispatcher
    add x16, x16, :lo12:__wine_unix_call_dispatcher
    blr x16
    adrp x16, unix_lr_returned
    mov w17, #1
    str w17, [x16, :lo12:unix_lr_returned]
    b unix_return_target

.global syscall_return_target
syscall_return_target:
    mov w17, #1
    adrp x16, syscall_x9_returned
    str w17, [x16, :lo12:syscall_x9_returned]
    b capture_return

.global invoke_callback
.type invoke_callback, %function
invoke_callback:
    save_host
    load_nonvolatile
    adrp x18, stale_teb
    add x18, x18, :lo12:stale_teb
    adrp x0, callback_target
    add x0, x0, :lo12:callback_target
    adrp x1, input_args
    add x1, x1, :lo12:input_args
    mov w2, #64
    adrp x3, authoritative_teb
    add x3, x3, :lo12:authoritative_teb
    bl wine_nx_call_pe_callback
    b capture_return

.type callback_target, %function
callback_target:
    adrp x16, callback_args
    add x16, x16, :lo12:callback_args
    stp x0, x1, [x16]
    str x18, [x16, #16]
    mov x23, #0x23
    mov x24, #0x24
    mov x28, #0x28
    mov x0, #42
    ret

.global syscall_dispatch_ret
syscall_dispatch_ret:
    adrp x16, dispatch_ret_called
    mov w17, #1
    str w17, [x16, :lo12:dispatch_ret_called]
    br x30

.global unix_return_target
unix_return_target:
    b capture_return

.type capture_return, %function
capture_return:
    adrp x16, observed_state
    add x16, x16, :lo12:observed_state
    str x0, [x16]
    str x18, [x16, #8]
    mov x17, sp
    str x17, [x16, #16]
    str x30, [x16, #24]
    adrp x16, observed_nonvolatile
    add x16, x16, :lo12:observed_nonvolatile
    stp x19, x20, [x16]
    stp x21, x22, [x16, #16]
    stp x23, x24, [x16, #32]
    stp x25, x26, [x16, #48]
    stp x27, x28, [x16, #64]
    str x29, [x16, #80]
    adrp x16, observed_vec
    add x16, x16, :lo12:observed_vec
    stp q6, q7, [x16]
    stp q8, q9, [x16, #32]
    stp q10, q11, [x16, #64]
    stp q12, q13, [x16, #96]
    stp q14, q15, [x16, #128]
    b test_return

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

.global clobber_vectors
.type clobber_vectors, %function
clobber_vectors:
    movi v6.16b, #0
    movi v7.16b, #0
    movi v8.16b, #0
    movi v9.16b, #0
    movi v10.16b, #0
    movi v11.16b, #0
    movi v12.16b, #0
    movi v13.16b, #0
    movi v14.16b, #0
    movi v15.16b, #0
    ret

.section .note.GNU-stack,"",%progbits
