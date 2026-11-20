// C = A * B with vx_mma, staged through shared memory.  A is MxK in bf16, by rows; Bt is B, KxN in bf16, by
// columns; C is MxN in fp32, by rows.  A word of A or Bt holds two elements.  M and N are multiples of 160,
// K of 64, and the three arrays start on a cache line.
//
//   void gemm_mma_kernel(const unsigned *A, const unsigned *Bt, float *C, int N, int K)
//
// A block is 160x160 of C, as 10x20 tiles of 16x8: 24 warps of 32 threads, six to a core.  Five of a core's
// six multiply: each owns 2x5 tiles, so it holds two tile rows of A, five tile columns of B and ten tiles of
// C in registers, and a tile's multiply costs it 1.8 loads.  C stays in registers until the end.  One warp of
// the block only fetches ahead; the other three sixths have nothing to do.
//
// K goes by in chunks of 64 columns, a cache line of each of the block's 160 rows of A and of Bt:
//   stage      each multiplying warp copies 8 rows of A and 8 of Bt to shared memory, a line a load
//   multiply   four k-tiles: the loads of the next k-tile's shares follow the multiplies that free their
//              registers, so the matrix unit never waits for one.  Meanwhile the fetching warp touches the
//              next chunk's lines, so that staging them finds them in the L1
// with a barrier after each.  A row in shared memory is 36 words apart from the next, so the 32 lanes of a
// load of a share (8 rows by 4 words) fall in 32 different banks; so do those of a staged line.
//
// What limits it is the L1's eight misses at a time, each held for a line's trip from DRAM, so the fetching
// warp keeps them busy: a line a load (a load of many lines books misses far ahead and wastes them), half of
// A before the SM running the block beside this one fetches it and half after, when it is in the L2, and the
// first of the block's lines of C once there is no chunk left to fetch.
//
// It is written in assembly to keep a thread to 74 registers, which six warps a core need: the compiler
// honours runs placed by hand for vx_mma, but spreads everything else over registers of its own (94 for
// this kernel in C).  The shares are in
//   x64-x71    A: tile row ti from x(64 + 4 ti), a register for rows + 8 hrow and columns + 8 hcol
//   x72-x81    B: tile column tj from x(72 + 2 tj)
//   x82-x121   C: tile (ti, tj) from x(82 + 4 (5 ti + tj))
// and everything else in registers _start has already written, which cost a warp nothing more
asm(R"(
    .section rad_kernels, "ax", @progbits
    .globl gemm_mma_kernel
    .type gemm_mma_kernel, @function
    .altmacro

    .macro bar
        .insn r 0x0b, 4, 0, x0, x0, x0
    .endm

    .macro lda_ reg, off
        lw.shared x\reg, \off(a6)
    .endm
    .macro lda ti, hrow, hcol, kq
        lda_ %(64 + 4*\ti + \hrow + 2*\hcol), %((16*\ti + 8*\hrow)*144 + (8*\kq + 4*\hcol)*4)
    .endm
    .macro lda4 ti, kq                 // a tile row of A at k-tile kq of the chunk
        lda \ti, 0, 0, \kq
        lda \ti, 1, 0, \kq
        lda \ti, 0, 1, \kq
        lda \ti, 1, 1, \kq
    .endm

    .macro ldb_ reg, off
        lw.shared x\reg, \off(a7)
    .endm
    .macro ldb2 tj, kq                 // a tile column of B at k-tile kq
        ldb_ %(72 + 2*\tj), %(23040 + 8*\tj*144 + 8*\kq*4)
        ldb_ %(73 + 2*\tj), %(23040 + 8*\tj*144 + (8*\kq + 4)*4)
    .endm

    .macro mma_ regc, rega, regb
        vx_mma.bf16 x\regc, x\rega, x\regb, x\regc
    .endm
    .macro mma ti, tj
        mma_ %(82 + 4*(5*\ti + \tj)), %(64 + 4*\ti), %(72 + 2*\tj)
    .endm

    .macro ktile nextq                 // a k-tile's ten multiplies, then k-tile nextq's loads as registers free
        mma 0, 0
        mma 0, 1
        mma 0, 2
        mma 0, 3
        mma 0, 4
        lda4 0, \nextq
        mma 1, 0
        ldb2 0, \nextq
        mma 1, 1
        ldb2 1, \nextq
        mma 1, 2
        ldb2 2, \nextq
        mma 1, 3
        ldb2 3, \nextq
        mma 1, 4
        ldb2 4, \nextq
        lda4 1, \nextq
    .endm

    // staging: row `row` of its 8 of A, then of its 8 of Bt, goes through register x(64 + row); its store
    // follows its load by three, since the line takes that long to arrive
    .macro stl_ reg
        lw   x\reg, 0(t1)
        add  t1, t1, a4
    .endm
    .macro stl row
        stl_ %(64 + \row)
    .endm
    .macro sts_ reg, off
        sw.shared x\reg, \off(a5)
    .endm
    .macro sts row, block, within      // row = 8 block + within; Bt's block is 160 rows after A's
        sts_ %(64 + \row), %(\block * 23040 + \within * 144)
    .endm

    .macro stc_ reg, off
        sw x\reg, \off(a2)
    .endm
    .macro stc ti, hrow, tj            // the two registers of C's tile (ti, tj) with rows + 8 hrow
        stc_ %(82 + 4*(5*\ti + \tj) + 2*\hrow), %(32*\tj)
        stc_ %(83 + 4*(5*\ti + \tj) + 2*\hrow), %(32*\tj + 4)
    .endm
    .macro stc5 ti, hrow
        stc \ti, \hrow, 0
        stc \ti, \hrow, 1
        stc \ti, \hrow, 2
        stc \ti, \hrow, 3
        stc \ti, \hrow, 4
        add a2, a2, a3
    .endm

    .macro touch groups                // the chunk's line of 8 * groups rows of A or Bt, from t1 on
        li   tp, \groups
    7:
        .rept 8
            lw   x0, 0(t1)
            add  t1, t1, a4
        .endr
        addi tp, tp, -1
        bnez tp, 7b
    .endm
    .macro fetch first, nfirst, second, nsecond   // a chunk ahead of the multiplying warps, from the second on
        beqz t0, 2f
    1:
        mv   t1, \first
        touch \nfirst
        mv   t1, a1
        touch 20
        mv   t1, \second
        touch \nsecond
        addi a0, a0, 128
        addi a1, a1, 128
        addi gp, gp, 128
        bar                            // the chunk before is multiplied
        bar                            // and this one staged
        addi t0, t0, -1
        bnez t0, 1b
    2:
        j    fetch_c
    .endm
    .macro touchc rows                 // the block's five lines of each of `rows` rows of C
        li   tp, \rows
    7:
        lw   x0, 0(a2)
        lw   x0, 128(a2)
        lw   x0, 256(a2)
        lw   x0, 384(a2)
        lw   x0, 512(a2)
        add  a2, a2, a3
        addi tp, tp, -1
        bnez tp, 7b
    .endm

gemm_mma_kernel:
    csrr t0, 0xfc7                     // this warp of the block
    li   t1, 6
    remu t2, t0, t1                    // its place among its core's six
    divu t0, t0, t1                    // its core's place among the block's four
    slli a4, a4, 1                     // bytes in a row of A or Bt
    li   t1, 160
    csrr a5, 0xfc4
    mul  a5, a5, t1                    // the block's first row of C
    csrr a6, 0xfc3
    mul  a6, a6, t1                    // and first column
    li   t1, 5
    beq  t2, t1, fetcher
    mul  t0, t0, t1
    add  t0, t0, t2                    // m: which of the twenty multiplying warps
    csrr t2, 0xfc6                     // lane

    // a0, a1: this chunk's line of the first of the 8 rows of A and of Bt it stages, at its lane's word
    slli t1, t0, 3
    add  a7, a5, t1
    mul  a7, a7, a4
    add  a0, a0, a7
    add  a7, a6, t1
    mul  a7, a7, a4
    add  a1, a1, a7
    slli a7, t2, 2
    add  a0, a0, a7
    add  a1, a1, a7
    // t1: where it stages them, A's block from row 0 of shared memory and Bt's from row 160
    csrr gp, 0xfca
    li   a7, 144
    mul  t1, t1, a7
    add  t1, t1, gp
    slli a7, t2, 2
    add  t1, t1, a7

    // its tiles: rows from 32 (m / 4) of the block, columns from 40 (m % 4)
    srli tp, t0, 2
    slli tp, tp, 5
    andi t0, t0, 3
    slli a7, t0, 5
    slli t0, t0, 3
    add  t0, t0, a7
    add  a6, a6, t0
    srli a7, t2, 2                     // g
    add  tp, tp, a7                    // the row its shares of A start at
    add  t0, t0, a7                    // the row (a column of B) its shares of Bt start at
    andi t2, t2, 3                     // t
    // a2: C at its row and column 2t of its first tile; a3: 8 rows of C
    add  a5, a5, tp
    mul  a5, a5, a3
    add  a5, a5, a6
    slli a7, t2, 1
    add  a5, a5, a7
    slli a5, a5, 2
    add  a2, a2, a5
    slli a3, a3, 5
    // a6, a7: its shares of A and Bt in shared memory
    li   a7, 144
    mul  tp, tp, a7
    mul  t0, t0, a7
    slli t2, t2, 2
    add  a6, gp, tp
    add  a6, a6, t2
    add  a7, gp, t0
    add  a7, a7, t2
    mv   a5, t1
    srli t0, a4, 7                     // chunks

    .irp reg, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101
        mv   x\reg, zero
    .endr
    .irp reg, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121
        mv   x\reg, zero
    .endr

chunk:
    // stage: 8 rows of A and 8 of Bt through the shares' own registers
    mv   t1, a0
    stl 0
    stl 1
    stl 2
    stl 3
    sts 0, 0, 0
    stl 4
    sts 1, 0, 1
    stl 5
    sts 2, 0, 2
    stl 6
    sts 3, 0, 3
    stl 7
    sts 4, 0, 4
    mv   t1, a1
    stl 8
    sts 5, 0, 5
    stl 9
    sts 6, 0, 6
    stl 10
    sts 7, 0, 7
    stl 11
    sts 8, 1, 0
    stl 12
    sts 9, 1, 1
    stl 13
    sts 10, 1, 2
    stl 14
    sts 11, 1, 3
    stl 15
    sts 12, 1, 4
    sts 13, 1, 5
    sts 14, 1, 6
    sts 15, 1, 7
    addi a0, a0, 128
    addi a1, a1, 128
    bar

    // multiply: the chunk's four k-tiles
    lda4 0, 0
    ldb2 0, 0
    ldb2 1, 0
    ldb2 2, 0
    ldb2 3, 0
    ldb2 4, 0
    lda4 1, 0
    ktile 1
    ktile 2
    ktile 3
    mma 0, 0
    mma 0, 1
    mma 0, 2
    mma 0, 3
    mma 0, 4
    mma 1, 0
    mma 1, 1
    mma 1, 2
    mma 1, 3
    mma 1, 4
    bar
    addi t0, t0, -1
    bnez t0, chunk

    // each register of C holds 8 rows by every other column of a tile
    stc5 0, 0
    stc5 0, 1
    stc5 1, 0
    stc5 1, 1
    ret

fetcher:
    bnez t0, done                      // one fetching warp is enough
    mul  tp, a5, a3
    add  tp, tp, a6
    slli tp, tp, 2
    add  a2, a2, tp                    // the block of C
    slli a3, a3, 2                     // a row of C on
    mul  a5, a5, a4
    add  a0, a0, a5
    addi a0, a0, 128                   // the second chunk's line of the block's first row of A
    mul  a6, a6, a4
    add  a1, a1, a6
    addi a1, a1, 128                   // and of Bt
    slli gp, a4, 6
    slli t1, a4, 5
    add  gp, gp, t1
    add  gp, gp, a0                    // and of its row 96 of A
    srli t0, a4, 7
    addi t0, t0, -1                    // chunks to fetch: the multiplying warps fetch the first as they stage it
    bar                                // which they have
    // rows 0-95 of A, Bt, then rows 96-159 of A; the SM running the block beside this one takes A the other way
    csrr t1, 0xfc3
    andi t1, t1, 1
    bnez t1, 3f
    fetch a0, 12, gp, 8
3:
    fetch gp, 8, a0, 12
fetch_c:
    touchc 52                          // what the last chunk's multiplies leave time for; the stores fetch the rest
    bar                                // the last chunk is multiplied: a warp must not leave while the rest wait
done:
    ret
    .size gemm_mma_kernel, . - gemm_mma_kernel
    .noaltmacro
)");
