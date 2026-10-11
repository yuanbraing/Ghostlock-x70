    .text
    .global ppoll_from_regs
    .type   ppoll_from_regs, %function

/*
 * long ppoll_from_regs(long regs[31]);
 *
 *   x0 = 指向 regs 数组的指针（对应调用者的 x0..x30 槽位）
 *
 * 数组索引与寄存器对应关系：
 *   regs[0]  -> x0   ... regs[7]  -> x7
 *   regs[8]  被忽略（由内核约定设为 __NR_ppoll）
 *   regs[9]  -> x9   ... regs[30] -> x30
 *
 * 返回值：系统调用返回的 x0（成功为 0/-1 等，出错为 -errno）
 */
ppoll_from_regs:
    /* 保存被调用者保存寄存器 x19-x28 以及 x29/fp、x30/lr */
    stp x19, x20, [sp, #-112]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    /* x19 作为数组基址，x0 即可复用 */
    mov x19, x0

    /* x0-x7 */
    ldp x0,  x1,  [x19, #0]
    ldp x2,  x3,  [x19, #16]
    ldp x4,  x5,  [x19, #32]
    ldp x6,  x7,  [x19, #48]

    /* x8 留空，稍后写入系统调用号 */

    /* x9-x18 */
    ldp x9,  x10, [x19, #72]
    ldp x11, x12, [x19, #88]
    ldp x13, x14, [x19, #104]
    ldp x15, x16, [x19, #120]
    ldp x17, x18, [x19, #136]

    /* x20-x29（先跳过 x19，它是基址） */
    ldp x20, x21, [x19, #160]
    ldp x22, x23, [x19, #176]
    ldp x24, x25, [x19, #192]
    ldp x26, x27, [x19, #208]
    ldp x28, x29, [x19, #224]

    /* x30 */
    ldr x30, [x19, #240]

    /* 最后加载 x19，此时基址已无用 */
    ldr x19, [x19, #152]

    /* x8 = __NR_ppoll（ARM64 上为 73） */
    mov x8, #73

    /* 陷入内核 */
    svc #0

    /* 返回值已在 x0；恢复被调用者保存寄存器 */
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #112

    ret
    .size ppoll_from_regs, .-ppoll_from_regs