/* ==========================================================================
 * cie-vdot-baseline.c  --  自定义 vdot.vv 的标准 RVV 基线实现（阶段三）
 *
 * 语义（与 vdot.vv 完全一致）：
 *   输入  A[0..15], B[0..15] 共 16 个 int8（= 一个 e8/m1 向量寄存器）
 *   输出  out[i] = sum_{k=0..3} sext8(A[4i+k]) * sext8(B[4i+k]),  i = 0..3
 *   32 位补码回绕，不饱和。测试口径：vl=16 / SEW=e8 / LMUL=1 / 无掩码。
 *
 * 三个版本（都是"有经验的 RVV 程序员"会写出的实现，均无标量乘加）：
 *
 *   gat  "两级 shift-add + vrgather"：                     核心 12 条
 *        vwmul.vv      (e8m1  -> e16m2) 16 个 int16 乘积
 *        vwadd.vv      (e16m2 -> e32m4) 符号扩展到 e32
 *        vslidedown.vi + vadd.vv  x2     t[j] = p[j]+p[j+1]+p[j+2]+p[j+3]
 *        vrgather.vv (idx = vid<<2)      把 lane 0/4/8/12 抽到 lane 0..3
 *        vse32.v                         4 个字直接写回
 *
 *   ngt  "同上但不用 gather"：                            核心 10 条
 *        结果仍在 lane 0/4/8/12，vse32.v 整组 16 个字存回暂存,
 *        再由标量取 4 个 word 写 out（多了 4 次标量 lw，少一条 vrgather）。
 *
 *   acc  "vwadd.wv 逐列累加"（不用 gather）：             核心 12 条
 *        vwmul.vv 得到 p16；
 *        vwadd.vv  把 p16 加 0 扩成 e32（k=0）；
 *        vslidedown.vi + vwadd.wv x3 逐个把 p[j+k] 加到宽累加器上。
 *        NEMU 未实现 vwadd.wv（会触发非法指令），只在 RTL emu 上跑，
 *        编译时 -DUSE_VWADD_WV=0 可关掉。
 *
 * 关于符号扩展：RVV 1.0 本应用 vsext.vf2(e16m2 -> e32m4)，但实测当前
 * emu(XiangShan kunminghu-v2 @7b16188) 的 vsext.vf2 只按低 8 位扩展
 * （0x0082 -> 0xFFFFFF82），是 RTL bug；故改用等价标准指令
 *     vwadd.vv vd(e32m4), vs2(e16m2), vzero(e16m2)   // vd = sext16(vs2) + 0
 *     vwadd.wv vd(e32m4), vs2(e16m2), vs1(e32m4)     // vd = vs1 + sext16(vs2)
 * 条数与原方案完全相同（各 1 条），不影响基线公平性。
 *
 * 编译宏：
 *   -DUSE_VWADD_WV=1  启用 acc 版本（emu 用；NEMU 用 0）
 *   -DVDOT_REF=1      额外用自定义 vdot.vv 算一遍做三方对比（需要支持 vdot 的模型）
 *   -DDUMP=1          打印每个用例的结果数组（NEMU 用；emu 上 printf 极慢）
 * ========================================================================== */
#include <klib.h>
#include <stdint.h>

#define VL      16
#define NCASE   10
#define NGRP    4
#define NTOT    (NCASE * NGRP)

#ifndef USE_VWADD_WV
#define USE_VWADD_WV 1
#endif
#ifndef VDOT_REF
#define VDOT_REF 0
#endif
#ifndef DUMP
#define DUMP 0
#endif

/* ---------------- 数据 ---------------- */
static uint8_t  A[VL]  __attribute__((aligned(16)));
static uint8_t  B[VL]  __attribute__((aligned(16)));
static int32_t  T1[VL] __attribute__((aligned(16)));   /* 整组存回暂存 */
static int32_t  OUT[VL]__attribute__((aligned(16)));   /* 输出（不相关版本用不同槽） */
static int32_t  R[4];

static volatile int mask0 = 0;      /* 恒 0，防止编译器把依赖链折掉 */

/* ---------------- 标量参考模型 ---------------- */
static int32_t sext8(uint8_t x) { return (int32_t)(int8_t)x; }

static int32_t ref_dot(const uint8_t *a, const uint8_t *b, int i)
{
    uint32_t s = 0;                 /* 32 位自然回绕 */
    for (int k = 0; k < 4; k++)
        s += (uint32_t)((int32_t)sext8(a[4 * i + k]) * (int32_t)sext8(b[4 * i + k]));
    return (int32_t)s;
}

/* ---------------- 周期计数 ---------------- */
static uint32_t rd_mcycle(void)
{
    uint32_t x;
    asm volatile("csrr %0, mcycle" : "=r"(x) :: "memory");
    return x;
}

/* ---------------- 索引向量 vid<<2（循环外建一次） ---------------- */
static void setup_idx(void)
{
    asm volatile("vsetivli t0,16,e32,m4,ta,ma\n\t"
                 "vid.v     v24\n\t"
                 "vsll.vi   v24,v24,2\n\t" ::: "t0", "memory");
}

/* ==========================================================================
 *  gat: vwmul + 两级 shift-add + vrgather        （核心 12 条）
 * ========================================================================== */
static void dot_gat(const uint8_t *a, const uint8_t *b, int32_t *out)
{
    asm volatile(
        "vsetivli   t0,16,e8,m1,ta,ma\n\t"       /* 1  vtype: e8/m1          */
        "vle8.v     v1,(%0)\n\t"                 /*    A[0..15]              */
        "vle8.v     v2,(%1)\n\t"                 /*    B[0..15]              */
        "vwmul.vv   v4,v1,v2\n\t"                /* 2  p16 = sext8(a)*sext8(b) (e16m2) */
        "vsetivli   t0,16,e16,m2,ta,ma\n\t"      /* 3  vtype: e16/m2         */
        "vmv.v.i    v6,0\n\t"                    /* 4  v6 = 0                */
        "vwadd.vv   v8,v4,v6\n\t"                /* 5  p32 = sext16(p16)     (e32m4)   */
        "vsetivli   t0,16,e32,m4,ta,ma\n\t"      /* 6  vtype: e32/m4         */
        "vslidedown.vi v12,v8,1\n\t"             /* 7                        */
        "vadd.vv    v16,v8,v12\n\t"              /* 8  s[j] = p[j]+p[j+1]    */
        "vslidedown.vi v12,v16,2\n\t"            /* 9                        */
        "vadd.vv    v8,v16,v12\n\t"              /* 10 t[j] = s[j]+s[j+2]    */
        "vrgather.vv v20,v8,v24\n\t"             /* 11 取 lane 0/4/8/12 -> 0..3 */
        "vsetivli   t0,4,e32,m1,ta,ma\n\t"       /* 12 vtype: e32/m1, vl=4   */
        "vse32.v    v20,(%2)\n\t"
        : : "r"(a), "r"(b), "r"(out) : "t0", "memory");
}

/* ==========================================================================
 *  ngt: 同 gat，但用整组存回 + 标量取（不用 vrgather）（核心 10 条）
 * ========================================================================== */
static void dot_ngt(const uint8_t *a, const uint8_t *b, int32_t *out)
{
    asm volatile(
        "vsetivli   t0,16,e8,m1,ta,ma\n\t"       /* 1  */
        "vle8.v     v1,(%0)\n\t"
        "vle8.v     v2,(%1)\n\t"
        "vwmul.vv   v4,v1,v2\n\t"                /* 2  */
        "vsetivli   t0,16,e16,m2,ta,ma\n\t"      /* 3  */
        "vmv.v.i    v6,0\n\t"                    /* 4  */
        "vwadd.vv   v8,v4,v6\n\t"                /* 5  */
        "vsetivli   t0,16,e32,m4,ta,ma\n\t"      /* 6  */
        "vslidedown.vi v12,v8,1\n\t"             /* 7  */
        "vadd.vv    v16,v8,v12\n\t"              /* 8  */
        "vslidedown.vi v12,v16,2\n\t"            /* 9  */
        "vadd.vv    v8,v16,v12\n\t"              /* 10 */
        "vse32.v    v8,(%2)\n\t"                 /* 11 整组 16 个字存回 */
        : : "r"(a), "r"(b), "r"(T1) : "t0", "memory");
    out[0] = T1[0]; out[1] = T1[4]; out[2] = T1[8]; out[3] = T1[12];
}

/* ==========================================================================
 *  acc: vwmul + vslidedown + vwadd.wv 逐列累加（核心 12 条）
 *  需要 vwadd.wv：emu 支持，NEMU 未实现（非法指令）
 * ========================================================================== */
#if USE_VWADD_WV
static void dot_acc(const uint8_t *a, const uint8_t *b, int32_t *out)
{
    asm volatile(
        "vsetivli   t0,16,e8,m1,ta,ma\n\t"       /* 1  */
        "vle8.v     v1,(%0)\n\t"
        "vle8.v     v2,(%1)\n\t"
        "vwmul.vv   v4,v1,v2\n\t"                /* 2  p16 (e16m2) */
        "vsetivli   t0,16,e16,m2,ta,ma\n\t"      /* 3  */
        "vmv.v.i    v6,0\n\t"                    /* 4  */
        "vwadd.vv   v8,v4,v6\n\t"                /* 5  acc0 = sext16(p)        k=0 */
        "vslidedown.vi v6,v4,1\n\t"              /* 6  */
        "vwadd.wv   v16,v6,v8\n\t"               /* 7  acc1 = acc0 + sext(p+1) k=1 */
        "vslidedown.vi v6,v4,2\n\t"              /* 8  */
        "vwadd.wv   v8,v6,v16\n\t"               /* 9  acc0 = acc1 + sext(p+2) k=2 */
        "vslidedown.vi v6,v4,3\n\t"              /* 10 */
        "vwadd.wv   v16,v6,v8\n\t"               /* 11 acc1 = acc0 + sext(p+3) k=3 */
        "vsetivli   t0,16,e32,m4,ta,ma\n\t"      /* 12 */
        "vse32.v    v16,(%2)\n\t"
        : : "r"(a), "r"(b), "r"(T1) : "t0", "memory");
    out[0] = T1[0]; out[1] = T1[4]; out[2] = T1[8]; out[3] = T1[12];
}
#endif

#if VDOT_REF
static void dot_vdot(const uint8_t *a, const uint8_t *b, int32_t *out)
{
    asm volatile("vsetivli t0,16,e8,m1,ta,ma\n\t"
                 "vle8.v v1,(%0)\n\t"
                 "vle8.v v2,(%1)\n\t"
                 ".insn 0x8220818b\n\t"                   /* vdot.vv v3,v1,v2 */
                 "vsetivli t0,4,e32,m1,ta,ma\n\t"
                 "vse32.v v3,(%2)\n\t"
                 :: "r"(a), "r"(b), "r"(out) : "t0", "memory");
}
#endif

/* ==========================================================================
 *  测试用例
 * ========================================================================== */
static const char *case_name[NCASE] = {
    "pos", "neg", "mix", "min*min", "bnd-7f/80", "min*-1",
    "0xfe*0xfe", "0xc0*0x40", "0x00/0xff", "rnd"
};

static void fill_case(int c, uint8_t *a, uint8_t *b)
{
    int i;
    switch (c) {
    case 0:  /* 全正数：1..16 * 64..79 */
        for (i = 0; i < VL; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(64 + i); }
        break;
    case 1:  /* 全负数：-16..-1 * -1..-16 */
        for (i = 0; i < VL; i++) { a[i] = (uint8_t)(0xF0 + i); b[i] = (uint8_t)(0xFF - i); }
        break;
    case 2:  /* 正负混合 */
        for (i = 0; i < VL; i++) {
            a[i] = (uint8_t)((i & 1) ? (0x80 | (i + 1)) : (i + 1));
            b[i] = (uint8_t)((i & 1) ? (i + 1) : (0x80 | (i + 1)));
        }
        break;
    case 3:  /* -128 * -128 = 16384，4 项和 = 65536 */
        for (i = 0; i < VL; i++) { a[i] = 0x80; b[i] = 0x80; }
        break;
    case 4:  /* 边界 0x7F / 0x80 / 0x00 / 0xFF */
        for (i = 0; i < VL; i++) {
            a[i] = (uint8_t)((i & 3) == 0 ? 0x7F : (i & 3) == 1 ? 0x80 :
                             (i & 3) == 2 ? 0x00 : 0xFF);
            b[i] = (uint8_t)((i & 3) == 0 ? 0x80 : (i & 3) == 1 ? 0x7F :
                             (i & 3) == 2 ? 0xFF : 0x00);
        }
        break;
    case 5:  /* -128 * -1 = 128 */
        for (i = 0; i < VL; i++) { a[i] = 0x80; b[i] = 0xFF; }
        break;
    case 6:  /* 0xFE(-2) * 0xFE(-2) = 4 */
        for (i = 0; i < VL; i++) { a[i] = 0xFE; b[i] = 0xFE; }
        break;
    case 7:  /* 0xC0(-64) * 0x40(64) = -4096 */
        for (i = 0; i < VL; i++) { a[i] = 0xC0; b[i] = 0x40; }
        break;
    case 8:  /* 0x00 / 0xFF */
        for (i = 0; i < VL; i++) { a[i] = (uint8_t)((i & 1) ? 0xFF : 0x00);
                                   b[i] = (uint8_t)((i & 1) ? 0xFF : 0x00); }
        break;
    default: /* 伪随机（LCG） */
        { uint32_t s = 0x12345678u;
          for (i = 0; i < VL; i++) { s = s * 1664525u + 1013904223u; a[i] = (uint8_t)(s >> 24);
                                     s = s * 1664525u + 1013904223u; b[i] = (uint8_t)(s >> 24); } }
        break;
    }
}

/* ==========================================================================
 *  正确性验证（逐元素与标量参考模型比对）
 * ========================================================================== */
typedef void (*dotfn)(const uint8_t *, const uint8_t *, int32_t *);

static void dump4(const char *tag, const int32_t *v)
{
    printf("  %s %d %d %d %d\n", tag, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
}

static int check_variant(dotfn k, const char *tag)
{
    int c, i, ok = 0, nbad = 0;

    for (c = 0; c < NCASE; c++) {
        fill_case(c, A, B);
        for (i = 0; i < NGRP; i++) R[i] = ref_dot(A, B, i);
        k(A, B, OUT);
        for (i = 0; i < NGRP; i++) {
            if (OUT[i] == R[i]) ok++;
            else if (++nbad <= 2) {
                printf("[FAIL %s] %s\n", tag, case_name[c]);
                dump4("act ", OUT);
                dump4("exp ", R);
            }
        }
    }
    printf("[%s] %d/%d\n", tag, ok, NTOT);
    return ok == NTOT;
}

#if DUMP
static void dump_all(void)
{
    int c, i;
    int32_t t[4];
    for (c = 0; c < NCASE; c++) {
        fill_case(c, A, B);
        for (i = 0; i < NGRP; i++) R[i] = ref_dot(A, B, i);
        printf("[c%d] %s\n", c, case_name[c]);
        dump4("ref ", R);
        dot_gat(A, B, t); dump4("gat ", t);
        dot_ngt(A, B, t); dump4("ngt ", t);
#if USE_VWADD_WV
        dot_acc(A, B, t); dump4("acc ", t);
#endif
#if VDOT_REF
        dot_vdot(A, B, t); dump4("vdot", t);
#endif
    }
}
#endif

/* ==========================================================================
 *  周期测量
 *    L (latency)     : 本轮 out[0] 参与下一轮输入地址，强制串行依赖
 *    T (throughput)  : 一轮 4 个互不相关的 kernel，无跨迭代依赖
 *    nop             : 同样的循环骨架但不做点积，用于扣除循环开销
 *  迭代次数用编译期常量数组 NSEED[]，改这里即可
 * ========================================================================== */
#define WARMUP 4

__attribute__((noinline))
static uint32_t bench_ser(dotfn k, int n, int32_t *out)
{
    uint32_t c0, c1; int i;
    const uint8_t *pa = A, *pb = B;
    for (i = 0; i < WARMUP; i++) k(A, B, out);
    c0 = rd_mcycle();
    for (i = 0; i < n; i++) {
        k(pa, pb, out);
        pa = A + ((uint32_t)out[0] & (uint32_t)mask0);   /* 依赖上一轮结果 */
        pb = B;
    }
    c1 = rd_mcycle();
    return c1 - c0;
}

__attribute__((noinline, unused))
static uint32_t bench_thru(dotfn k, int n, int32_t *out)
{
    uint32_t c0, c1; int i;
    int32_t *o0 = out, *o1 = out + 4, *o2 = out + 8, *o3 = out + 12;
    for (i = 0; i < WARMUP; i++) { k(A, B, o0); k(A, B, o1); k(A, B, o2); k(A, B, o3); }
    c0 = rd_mcycle();
    for (i = 0; i < n; i++) {
        k(A, B, o0); k(A, B, o1); k(A, B, o2); k(A, B, o3);
    }
    c1 = rd_mcycle();
    return c1 - c0;
}

__attribute__((noinline))
static uint32_t bench_null(int n, int32_t *out)
{
    uint32_t c0, c1; int i;
    const uint8_t *pa = A, *pb = B;
    c0 = rd_mcycle();
    for (i = 0; i < n; i++) {
        pa = A + ((uint32_t)out[0] & (uint32_t)mask0);
        pb = B;
    }
    c1 = rd_mcycle();
    __asm__ volatile("" :: "r"(pa), "r"(pb));
    return c1 - c0;
}

/* 迭代次数：编译期常量数组，可用 -DNSEED_OVERRIDE='{4,8}' 覆盖 */
#ifdef NSEED_OVERRIDE
static const int NSEED[] = NSEED_OVERRIDE;
#else
static const int NSEED[] = { 100, 1000 };
#endif

int main(void)
{
    int ok = 1, j;
    const int nn = (int)(sizeof(NSEED) / sizeof(NSEED[0]));

    setup_idx();

#if DUMP
    dump_all();
#endif

    /* ---- 可移植的两个版本（只用 NEMU 也支持的指令）---- */
    ok &= check_variant(dot_gat, "gat");
    ok &= check_variant(dot_ngt, "ngt");

    for (j = 0; j < nn; j++) {
        int n = NSEED[j];
        int nul = (int)bench_null(n, OUT);
        int gL = (int)bench_ser(dot_gat, n, OUT);
        int nL = (int)bench_ser(dot_ngt, n, OUT);
#if BENCH_THRU
        int gT = (int)bench_thru(dot_gat, n, OUT);
        int nT = (int)bench_thru(dot_ngt, n, OUT);
#else
        int gT = 0, nT = 0;   /* 只测延迟：thru 被裁掉 */
#endif
        printf("[B] N=%d nop=%d gat=%d,%d ngt=%d,%d\n", n, nul, gL, gT, nL, nT);
    }

#if USE_VWADD_WV
    /* ---- 只在实现了 vwadd.wv 的模型上跑（emu 可以，NEMU 不行）---- */
    ok &= check_variant(dot_acc, "acc");
    for (j = 0; j < nn; j++) {
        int n = NSEED[j];
        int aL = (int)bench_ser(dot_acc, n, OUT);
#if BENCH_THRU
        int aT = (int)bench_thru(dot_acc, n, OUT);
#else
        int aT = 0;
#endif
        printf("[BA] N=%d acc=%d,%d\n", n, aL, aT);
    }
#endif
    return ok ? 0 : 1;
}
