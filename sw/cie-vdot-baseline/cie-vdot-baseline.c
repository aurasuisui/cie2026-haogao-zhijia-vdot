/* ============================================================================
 * 阶段三：cie-vdot-baseline
 *
 * 用「标准 RVV 1.0/1.1 指令」实现与自定义指令 vdot.vv 完全等价的运算，
 * 作为 latency 加速比的分母（对比基线）。
 *
 * 语义（与 vdot.vv 完全一致，本基线只做 vl=16 / SEW=e8 / LMUL=1 / 无掩码）：
 *     out[i] = sum_{k=0..3} sext8(A[4i+k]) * sext8(B[4i+k]),  i = 0..3
 *     32 位补码回绕，不饱和
 *
 * 实现思路（一个"有经验的 RVV 程序员"会写出的合理实现）：
 *   [e8m1]    vle8.v    取 A、B 各 16 字节
 *   [e8m1]    vwmul.vv  逐元素有符号乘，e8 -> e16（products 最大值 16384，int16 不溢出）
 *   [e16m2]   vsext.vf2 符号扩展 e16 -> e32（16 个 int32 乘积占 v8..v11）
 *   [e32m4]   vslidedown.vi 1 + vadd.vv  -> s[j] = p[j] + p[j+1]
 *             vslidedown.vi 2 + vadd.vv  -> t[j] = p[j]+p[j+1]+p[j+2]+p[j+3]
 *             （必须在 e32 里累加：4 项最大 65536，int16 会溢出）
 *   [e32m4]   vrgather.vv 把 lane 0/4/8/12 收到 lane 0..3（索引向量 vid<<2，循环外建一次）
 *   [e32m1]   vse32.v    写回 4 个 int32
 *
 * 核心序列 11 条（不含 2 条 vle8.v / 1 条 vse32.v / 3 条一次性的索引向量建立）。
 *
 * 另外附带：标量参考模型逐元素比对、mcycle 周期测量（依赖链 / 吞吐两种口径）。
 * ==========================================================================*/
#include <klib.h>
#include <stdint.h>

#define VL      16
#define NGROUP  4

/* 探针版本可以把 bench 的规模压小（sed 替换上面两个宏即可） */
#ifndef RUN_BENCH
#define RUN_BENCH 1
#endif
#ifndef BENCH_N1
#define BENCH_N1 100
#endif
#ifndef BENCH_N2
#define BENCH_N2 1000
#endif
#ifndef BENCH_REP1
#define BENCH_REP1 2
#endif
#ifndef BENCH_REP2
#define BENCH_REP2 1
#endif

static uint8_t  A[VL]        __attribute__((aligned(16)));
static uint8_t  B[VL]        __attribute__((aligned(16)));
static uint32_t OUT[NGROUP]  __attribute__((aligned(16)));

static int checks = 0;
static int fails  = 0;

static volatile int      mask0 = 0;   /* 恒为 0，仅用于让下一轮的指针真的依赖上一轮结果 */
static volatile uint32_t sink  = 0;   /* 防止编译器把循环优化掉 */

/* ---------------------------------------------------------------- *
 * 一次点积的向量核心序列
 * 寄存器约定（EMUL>1 的组必须按组宽对齐）：
 *   v1,v2       : A,B (e8m1)
 *   v4:v5       : 16 x int16 乘积        (vwmul  e8m1 -> e16m2, 偶数对齐)
 *   v8..v11     : 16 x int32 乘积/前缀和 (vsext  e16m2 -> e32m4, 4 对齐)
 *   v12..v15    : slide 临时组
 *   v16..v19    : s[j] = p[j]+p[j+1]
 *   v20..v23    : gather 结果
 *   v24..v27    : 索引向量 (vid<<2)，循环外建立
 * ---------------------------------------------------------------- */
#define DOT_KERNEL(PA, PB, PO)                                        \
    asm volatile(                                                     \
        "vsetivli   t0, 16, e8, m1, ta, ma\n\t"                       \
        "vle8.v     v1, (%0)\n\t"                                     \
        "vle8.v     v2, (%1)\n\t"                                     \
        "vwmul.vv   v4, v1, v2\n\t"        /* p[j] = a[j]*b[j], e16 */\
        "vsetivli   t0, 16, e16, m2, ta, ma\n\t"                      \
        "vsext.vf2  v8, v4\n\t"            /* p -> e32 */             \
        "vsetivli   t0, 16, e32, m4, ta, ma\n\t"                      \
        "vslidedown.vi v12, v8, 1\n\t"                                \
        "vadd.vv    v16, v8, v12\n\t"      /* s[j]=p[j]+p[j+1] */     \
        "vslidedown.vi v12, v16, 2\n\t"                               \
        "vadd.vv    v8, v16, v12\n\t"      /* t[4i]=sum of 4 */       \
        "vrgather.vv v20, v8, v24\n\t"     /* lanes 0/4/8/12 -> 0..3 */\
        "vsetivli   t0, 4, e32, m1, ta, ma\n\t"                       \
        "vse32.v    v20, (%2)\n\t"                                    \
        :                                                             \
        : "r"(PA), "r"(PB), "r"(PO)                                   \
        : "t0", "memory")

/* 索引向量一次性建立：idx[j] = 4*j（>=VLMAX 的索引 vrgather 会当 0，无副作用） */
#define SETUP_IDX()                                                   \
    asm volatile(                                                     \
        "vsetivli t0, 16, e32, m4, ta, ma\n\t"                        \
        "vid.v    v24\n\t"                                            \
        "vsll.vi  v24, v24, 2\n\t"                                    \
        ::: "t0", "memory")

/* ---------------------------------------------------------------- *
 * 标量参考模型
 * ---------------------------------------------------------------- */
static int32_t ref_dot(const uint8_t *a, const uint8_t *b, int i)
{
    uint32_t s = 0;
    int k;
    for (k = 0; k < 4; k++) {
        int32_t x = (int32_t)(int8_t)a[4 * i + k];
        int32_t y = (int32_t)(int8_t)b[4 * i + k];
        s = s + (uint32_t)(x * y);          /* 32 位补码回绕 */
    }
    return (int32_t)s;
}

static inline uint32_t rd_mcycle(void)
{
    uint32_t x;
    asm volatile("csrr %0, mcycle" : "=r"(x) : : "memory");
    return x;
}

/* ---------------------------------------------------------------- *
 * 数据图案
 * ---------------------------------------------------------------- */
static void fill(uint8_t *p, int mode, int seed)
{
    static const uint8_t pat8[8] = {0x7f, 0x80, 0x00, 0xff, 0x01, 0xfe, 0x40, 0xc0};
    uint32_t x = (uint32_t)seed * 1103515245u + 12345u;
    int i;
    for (i = 0; i < VL; i++) {
        switch (mode) {
        case 0:  p[i] = (uint8_t)(i + 1);                       break; /* 小正数   */
        case 1:  p[i] = (uint8_t)(0x40 + (i & 0x3f));           break; /* 大正数   */
        case 2:  p[i] = (uint8_t)(0x80 | (i & 0x7f));           break; /* -128..-1 */
        case 3:  p[i] = 0x80;                                   break; /* -128     */
        case 4:  p[i] = 0xff;                                   break; /* -1       */
        case 5:  p[i] = 0x7f;                                   break; /* 127      */
        case 6:  p[i] = 0x00;                                   break; /* 0        */
        case 7:  p[i] = pat8[i & 7];                            break; /* 边界混合 */
        case 8:  x = x * 1103515245u + 12345u;
                 p[i] = (uint8_t)(x >> 16);                     break; /* 伪随机   */
        case 9:  p[i] = 0xfe;                                   break; /* -2       */
        case 10: p[i] = 0xc0;                                   break; /* -64      */
        case 11: p[i] = 0x40;                                   break; /* 64       */
        default: p[i] = (uint8_t)i;                             break;
        }
    }
}

/* ---------------------------------------------------------------- *
 * 单个用例：跑一次基线，逐元素与参考模型比对
 * ---------------------------------------------------------------- */
static void check_case(const char *name, int ma, int sa, int mb, int sb)
{
    int i;
    fill(A, ma, sa);
    fill(B, mb, sb);
    for (i = 0; i < NGROUP; i++) OUT[i] = 0xdeadbeefu;

    DOT_KERNEL((const uint8_t *)A, (const uint8_t *)B, (uint32_t *)OUT);
    asm volatile("" ::: "memory");

    for (i = 0; i < NGROUP; i++) {
        int32_t exp = ref_dot(A, B, i);
        int32_t act = (int32_t)OUT[i];
        checks++;
        if (act != exp) {
            fails++;
            if (fails <= 12)
                printf("  FAIL [%s] group %d: act=%08x exp=%08x\n",
                       name, i, (unsigned)act, (unsigned)exp);
        }
    }
}

/* ---------------------------------------------------------------- *
 * 周期测量
 *   bench_serial : 每次点积的地址依赖上一轮结果 -> 整条链被串行化
 *   bench_null   : 同样的循环骨架，但没有向量序列（扣循环开销用）
 *   bench_thru   : 输入不变、地址不变，背靠背发射（吞吐口径）
 * ---------------------------------------------------------------- */
#if RUN_BENCH
__attribute__((noinline))
static uint32_t bench_null(int n)
{
    const uint8_t *pa = A;
    uint32_t c0, c1;
    asm volatile("" ::: "memory");
    c0 = rd_mcycle();
    for (int i = 0; i < n; i++) {
        /* 与 bench_serial 完全相同的标量胶水：load OUT[0] + load mask0 + and + add，
         * 只是没有向量序列（volatile 读保证每轮真的发一次，不被提出循环） */
        pa = A + ((int32_t)(*(volatile uint32_t *)&OUT[0]) & mask0);
        asm volatile("" ::: "memory");
    }
    c1 = rd_mcycle();
    asm volatile("" ::: "memory");
    sink = (uint32_t)(pa - A);
    return c1 - c0;
}

__attribute__((noinline))
static uint32_t bench_serial(int n)
{
    const uint8_t *pa = A;
    uint32_t c0, c1;
    asm volatile("" ::: "memory");
    c0 = rd_mcycle();
    for (int i = 0; i < n; i++) {
        DOT_KERNEL(pa, (const uint8_t *)B, (uint32_t *)OUT);
        pa = A + ((int32_t)OUT[0] & mask0);   /* mask0==0，但依赖是真的 */
    }
    c1 = rd_mcycle();
    asm volatile("" ::: "memory");
    sink = (uint32_t)(pa - A) + OUT[0];
    return c1 - c0;
}

__attribute__((noinline))
static uint32_t bench_thru(int n)
{
    uint32_t c0, c1;
    asm volatile("" ::: "memory");
    c0 = rd_mcycle();
    for (int i = 0; i < n; i++)
        DOT_KERNEL((const uint8_t *)A, (const uint8_t *)B, (uint32_t *)OUT);
    c1 = rd_mcycle();
    asm volatile("" ::: "memory");
    sink = OUT[0] + OUT[3];
    return c1 - c0;
}

static uint32_t best(uint32_t *v, int n)
{
    uint32_t m = v[0];
    for (int i = 1; i < n; i++) if (v[i] < m) m = v[i];
    return m;
}

static void bench_report(const char *tag, uint32_t cyc, uint32_t nul, int n)
{
    /* 每点积周期，用整数打印到 0.01： (cyc-nul)*100/n */
    uint32_t per100 = (uint32_t)(((uint64_t)(cyc - nul) * 100u) / (uint32_t)n);
    printf("[BENCH] %s N=%-4d total=%u null=%u -> %u.%02u cyc/dot\n",
           tag, n, (unsigned)cyc, (unsigned)nul,
           (unsigned)(per100 / 100u), (unsigned)(per100 % 100u));
}
#endif

/* ---------------------------------------------------------------- */
int main(void)
{
    int i;

    printf("=== cie-vdot-baseline: RVV 1.0 equivalent of vdot.vv ===\n");

    SETUP_IDX();

    /* ---------------- 正确性 ---------------- */
    check_case("pos",        0, 0,  1, 0);
    check_case("pos-max",    5, 0,  5, 0);   /* 127*127*4 = 64516  */
    check_case("neg",        2, 0,  2, 0);
    check_case("min*min",    3, 0,  3, 0);   /* 65536  */
    check_case("min*-1",     3, 0,  4, 0);   /* 512    */
    check_case("min*127",    3, 0,  5, 0);   /* -65024 */
    check_case("bnd*bnd",    7, 0,  7, 0);
    check_case("bnd*min",    7, 0,  3, 0);
    check_case("zero",       6, 0,  6, 0);
    check_case("0xfe*0xfe",  9, 0,  9, 0);
    check_case("0xc0*0x40", 10, 0, 11, 0);
    check_case("rnd-1",      8, 1,  8, 2);
    check_case("rnd-2",      8, 3,  8, 4);
    check_case("rnd-3",      8, 5,  8, 6);
    check_case("rnd-4",      8, 7,  7, 0);

    if (fails == 0) {
        printf("[BASELINE] PASS %d/%d\n", checks, checks);
        /* 抽一个用例把数值打出来，方便人眼核对（-128 * -128 * 4 = 65536） */
        fill(A, 3, 0); fill(B, 3, 0);
        DOT_KERNEL((const uint8_t *)A, (const uint8_t *)B, (uint32_t *)OUT);
        asm volatile("" ::: "memory");
        printf("  spot: A=B=-128 -> out[0..3] = %d %d %d %d (exp 65536)\n",
               (int)OUT[0], (int)OUT[1], (int)OUT[2], (int)OUT[3]);
    } else {
        printf("[BASELINE] FAIL %d/%d\n", checks - fails, checks);
    }

#if RUN_BENCH
    /* ---------------- 周期测量 ---------------- */
    {
        uint32_t nul_v[BENCH_REP1 > BENCH_REP2 ? BENCH_REP1 : BENCH_REP2];
        uint32_t s1[BENCH_REP1], t1[BENCH_REP1];
        uint32_t s2[BENCH_REP2], t2[BENCH_REP2];
        uint32_t nul1, nul2;

        /* 数据用固定图案，避免 cache miss 抖动 */
        fill(A, 8, 11);
        fill(B, 8, 22);

        for (i = 0; i < BENCH_REP1; i++) nul_v[i] = bench_null(BENCH_N1);
        nul1 = best(nul_v, BENCH_REP1);
        for (i = 0; i < BENCH_REP2; i++) nul_v[i] = bench_null(BENCH_N2);
        nul2 = best(nul_v, BENCH_REP2);

        for (i = 0; i < BENCH_REP1; i++) s1[i] = bench_serial(BENCH_N1);
        for (i = 0; i < BENCH_REP1; i++) t1[i] = bench_thru(BENCH_N1);
        for (i = 0; i < BENCH_REP2; i++) s2[i] = bench_serial(BENCH_N2);
        for (i = 0; i < BENCH_REP2; i++) t2[i] = bench_thru(BENCH_N2);

        bench_report("serial", best(s1, BENCH_REP1), nul1, BENCH_N1);
        bench_report("serial", best(s2, BENCH_REP2), nul2, BENCH_N2);
        bench_report("thru  ", best(t1, BENCH_REP1), nul1, BENCH_N1);
        bench_report("thru  ", best(t2, BENCH_REP2), nul2, BENCH_N2);
        printf("[BENCH] done (rep1=%d rep2=%d, min of reps)\n", BENCH_REP1, BENCH_REP2);
    }
#endif

    printf("=== baseline done: %s ===\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
