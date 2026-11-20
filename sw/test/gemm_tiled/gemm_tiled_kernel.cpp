// C = A * B, one fmadd.s a multiply-add, staged through shared memory.  A is MxK in bf16, by rows; Bt is B,
// KxN in bf16, by columns; C is MxN in fp32, by rows.  A word of A or Bt holds two elements, and a row of
// either is LD elements from the next.  M is a multiple of 320, N of 128, K of 64; the three arrays start
// on a cache line, and LD is 16 more than a multiple of 64: a row then starts a quarter of a line further
// into its line than the row before, so a quarter of the rows move on to a new line every 16 steps of k,
// and a block's lines spread over all the sets of the L1.
//
//   void gemm_tiled_kernel(const unsigned *A, const unsigned *Bt, float *C, int N, int K, int LD)
//
// A thread owns 10 rows by 8 columns of C and keeps all 80 sums in registers, so a step of k costs it 18
// loads (10 elements of A, 8 of B) for 80 multiply-adds.  A warp is 8 by 4 threads, 80 rows by 32 columns; a
// block is 4 by 4 warps, 320 by 128, four warps to a core.
//
// Shared memory holds the block's operands for 16 steps of k, widened to fp32, as a panel: the word for step
// k and operand i (A's 320 rows, then B's 128 columns) is at 454 k + i.  The threads of a warp that load an
// operand then fall in different banks (8 rows 10 apart, or 4 columns 8 apart), and so do the 32 lanes of a
// store that stages 4 rows by 8 steps.  There are two panels: while the block multiplies from one, each warp
// stages the next 16 steps of 20 rows of A and 8 of Bt into the other, a little in each pass over two steps,
// and a barrier swaps them.  A staging load reads 4 rows, one of them from a line not seen before; its
// store comes a pass later, when that line has long arrived, so nothing waits for a trip from DRAM.
//
// Registers: x18-x27 the 10 elements of A, x28-x35 the 8 of B, x(36 + 8 r + c) the sum for row r, column c.
// It is in assembly to place them and to order the loads; a pass is a subroutine.
asm(R"(
    .section rad_kernels, "ax", @progbits
    .globl gemm_tiled_kernel
    .type gemm_tiled_kernel, @function
    .altmacro

    .macro bar
        .insn r 0x0b, 4, 0, x0, x0, x0
    .endm

    .macro fma_ sum, ela, elb
        fmadd.s x\sum, x\ela, x\elb, x\sum
    .endm
    .macro fma r, c
        fma_ %(36 + 8*\r + \c), %(18 + \r), %(28 + \c)
    .endm
    .macro lda_ reg, off
        lw.shared x\reg, \off(a6)
    .endm
    .macro lda r, step                 // A's element r, `step` steps of k on from a6
        lda_ %(18 + \r), %(4*\r + 1816*\step)
    .endm
    .macro ldb_ reg, off
        lw.shared x\reg, \off(a7)
    .endm
    .macro ldb c, step
        ldb_ %(28 + \c), %(4*\c + 1816*\step)
    .endm

    // a step of k: a row's 8 multiply-adds, then the load of that row's element of A for the step after;
    // B's elements are free only in the last row
    .macro row r, next
        fma \r, 0
        fma \r, 1
        fma \r, 2
        fma \r, 3
        fma \r, 4
        fma \r, 5
        fma \r, 6
        fma \r, 7
        lda \r, \next
    .endm
    .macro kstep next
        row 0, \next
        row 1, \next
        row 2, \next
        row 3, \next
        row 4, \next
        row 5, \next
        row 6, \next
        row 7, \next
        row 8, \next
        fma 9, 0
        ldb 0, \next
        fma 9, 1
        ldb 1, \next
        fma 9, 2
        ldb 2, \next
        fma 9, 3
        ldb 3, \next
        fma 9, 4
        ldb 4, \next
        fma 9, 5
        ldb 5, \next
        fma 9, 6
        ldb 6, \next
        fma 9, 7
        ldb 7, \next
        lda 9, \next
    .endm

    .macro row8 r
        fma \r, 0
        fma \r, 1
        fma \r, 2
        fma \r, 3
        fma \r, 4
        fma \r, 5
        fma \r, 6
        fma \r, 7
    .endm
    .macro klast                       // a panel's last step of k: nothing of it is left to load
        row8 0
        row8 1
        row8 2
        row8 3
        row8 4
        row8 5
        row8 6
        row8 7
        row8 8
        row8 9
    .endm

    // staging: widen and store the 4 rows by 8 steps x8 holds (a word is two elements), and load the next 4
    // rows
    .macro stage
        slli x9, x8, 16
        and  x8, x8, x122
        sw.shared x9, 0(t2)
        sw.shared x8, 1816(t2)
        addi t2, t2, 16
        lw   x8, 0(t0)
        add  t0, t0, a4
    .endm
    // the eight passes of staging 16 steps: 20 rows of A (from a0) and 8 of Bt (from a1) into the panel at
    // gp.  `pass` is what a pass does besides, and `last` what the last one does
    .macro stage16 pass, last
        mv   t0, a0
        add  t2, gp, x120              // nothing is loaded yet: the first store goes to spare words
        \pass
        mv   t2, gp
        \pass
        \pass
        \pass
        \pass
        mv   t0, a1
        \pass
        add  t2, gp, x119
        \pass
        \last
    .endm
    .macro call_body
        jal  ra, body
    .endm
    .macro call_last
        jal  ra, body_last
    .endm

    // x116 times: multiply 16 steps from one panel and stage the next 16 into the other
    .macro sixteens
    1:
        addi a0, a0, 32
        addi a1, a1, 32
        mv   a6, x117
        add  a7, x117, x118
        lda 0, 0
        lda 1, 0
        lda 2, 0
        lda 3, 0
        lda 4, 0
        lda 5, 0
        lda 6, 0
        lda 7, 0
        lda 8, 0
        lda 9, 0
        ldb 0, 0
        ldb 1, 0
        ldb 2, 0
        ldb 3, 0
        ldb 4, 0
        ldb 5, 0
        ldb 6, 0
        ldb 7, 0
        stage16 call_body, call_last
        bar
        add  x117, x117, tp            // the panels swap
        sub  gp, gp, tp
        sub  tp, zero, tp
        addi x116, x116, -1
        bnez x116, 1b
    .endm

    .macro stc_ reg, off
        sw x\reg, \off(a2)
    .endm
    .macro stc r, c
        stc_ %(36 + 8*\r + \c), %(4*\c)
    .endm
    .macro stcrow r
        stc \r, 0
        stc \r, 1
        stc \r, 2
        stc \r, 3
        stc \r, 4
        stc \r, 5
        stc \r, 6
        stc \r, 7
        add a2, a2, a3
    .endm

gemm_tiled_kernel:
    srli x116, a4, 4                   // sixteens of steps
    slli a4, a5, 1                     // bytes from a row of A or Bt to the next
    csrr t0, 0xfc7                     // w: this warp of the block's 16
    csrr t1, 0xfc6                     // lane
    csrr a5, 0xfc4
    li   a6, 320
    mul  a5, a5, a6                    // the block's first row of C
    csrr a6, 0xfc3
    slli a6, a6, 7                     // and first column
    srli t2, t1, 3                     // q: which of the 4 rows of a staging load
    andi x8, t1, 7                     // and which of its 8 words
    // a0, a1: the first 16 steps of the first of the 20 rows of A and of the 8 of Bt it stages
    li   a7, 20
    mul  a7, a7, t0
    add  a7, a7, a5
    add  a7, a7, t2
    mul  a7, a7, a4
    add  a0, a0, a7
    slli a7, x8, 2
    add  a0, a0, a7
    slli x9, t0, 3
    add  x9, x9, a6
    add  x9, x9, t2
    mul  x9, x9, a4
    add  a1, a1, x9
    add  a1, a1, a7
    // a7: where it stages A in the first panel: steps 2 word and 2 word + 1, operands 20 w + q on
    csrr gp, 0xfca
    li   a7, 3632
    mul  a7, a7, x8
    add  a7, a7, gp
    li   x9, 80
    mul  x9, x9, t0
    add  a7, a7, x9
    slli x8, t2, 2
    add  a7, a7, x8
    // from there to where it stages Bt (operands 320 + 8 w on), and to spare words (operands 448 on)
    li   x119, 48
    mul  x119, x119, t0
    li   x8, 1280
    sub  x119, x8, x119
    li   x8, 1792
    sub  x120, x8, x9
    // its rows of the block from 80 (w / 4) + 10 (lane / 4), its columns from 32 (w % 4) + 8 (lane % 4)
    srli x8, t0, 2
    andi t0, t0, 3
    srli x9, t1, 2
    andi t1, t1, 3
    li   t2, 80
    mul  t2, t2, x8
    li   x8, 10
    mul  x9, x9, x8
    add  t2, t2, x9
    slli t0, t0, 5
    slli t1, t1, 3
    add  t0, t0, t1
    // x117: its elements of A in the first panel; x118: from there to its elements of B
    slli x8, t2, 2
    add  x117, gp, x8
    addi x118, t0, 320
    sub  x118, x118, t2
    slli x118, x118, 2
    // a2: its corner of C; a3: a row of C
    add  a5, a5, t2
    mul  a5, a5, a3
    add  a5, a5, a6
    add  a5, a5, t0
    slli a5, a5, 2
    add  a2, a2, a5
    slli a3, a3, 2
    mv   gp, a7
    li   tp, 29056                     // a panel: 16 steps of 454 words
    slli a4, a4, 2                     // 4 rows of A or Bt
    li   x122, 0xffff0000              // the second element of a word, widened

    .irp reg, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55
        mv   x\reg, zero
    .endr
    .irp reg, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75
        mv   x\reg, zero
    .endr
    .irp reg, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95
        mv   x\reg, zero
    .endr
    .irp reg, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115
        mv   x\reg, zero
    .endr

    stage16 stage, stage               // the first 16 steps, into the first panel
    bar
    add  gp, gp, tp

    sixteens

    // each sum's register holds 8 rows by 4 columns of C, 8 columns apart
    stcrow 0
    stcrow 1
    stcrow 2
    stcrow 3
    stcrow 4
    stcrow 5
    stcrow 6
    stcrow 7
    stcrow 8
    stcrow 9
    mv   ra, zero
    ret

body:                                  // a pass: two steps of k and an eighth of the staging
    kstep 1
    kstep 2
    addi a6, a6, 3632
    addi a7, a7, 3632
    stage
    ret
body_last:                             // the last pass over a panel
    kstep 1
    klast
    stage
    ret
    .size gemm_tiled_kernel, . - gemm_tiled_kernel
    .noaltmacro
)");
