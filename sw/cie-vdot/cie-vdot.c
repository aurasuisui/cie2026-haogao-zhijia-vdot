/* 阶段二：自定义向量指令 vdot.vv 的三组测试
 *
 *   vdot.vv vd, vs2, vs1   (custom-0, funct6=100000, funct3=000)
 *   vd[32i+31:32i] = sum_{k=0..3} int8(vs2[8(4i+k)]) * int8(vs1[8(4i+k)])
 *   前置条件 vtype.vsew = e8, vlmul = m1;  vl 以源字节计数; 输出有效个数 = ceil(vl/4)
 *   mask(vm=0) 与 tail 按 32 位输出元素粒度: 输出元素 i 用 v0[i]
 *
 * 编码: enc = (funct7<<25)|(vs2<<20)|(vs1<<15)|(funct3<<12)|(vd<<7)|0x0B, funct7 = funct6<<1|vm
 *       vdot.vv v3, v2, v1, vm=1  -> 0x8220818b
 *       vdot.vv v3, v2, v1, vm=0  -> 0x8020818b
 *
 * GNU as 对向量自定义指令的 .insn r 形式会报 "bad value for funct2 field", 只能整条写死.
 */
#include <klib.h>
#include <stdint.h>

#define VL 16

#define INS_VDOT_VM1 0x8220818b
#define INS_VDOT_VM0 0x8020818b

static uint8_t A[VL] __attribute__((aligned(16)));
static uint8_t B[VL] __attribute__((aligned(16)));
static uint8_t OLD[VL] __attribute__((aligned(16)));
static uint8_t OUT[VL] __attribute__((aligned(16)));

static int checks = 0;
static int fails = 0;
static const char *cur_case = "";

/* ---------------- 标量参考模型 ---------------- */
static int32_t sext8(uint8_t x) { return (int32_t)(int8_t)x; }

static int32_t ref_dot(const uint8_t *a, const uint8_t *b, int i)
{
    int32_t s = 0;
    for (int k = 0; k < 4; k++)
        s += sext8(a[4 * i + k]) * sext8(b[4 * i + k]);
    return s;
}

static int32_t rd32(const uint8_t *p, int i)
{
    /* 小端: 元素 i 占字节 4i..4i+3 */
    uint32_t v = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8)
               | ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    return (int32_t)v;
}

/* ---------------- vsetvli 四种 ta/ma 组合 ---------------- */
static size_t vset_e8(int vl, int ta, int ma)
{
    size_t r = 0;
    if (ta && ma)
        asm volatile("vsetvli %0, %1, e8, m1, ta, ma" : "=r"(r) : "r"((long)vl));
    else if (ta && !ma)
        asm volatile("vsetvli %0, %1, e8, m1, ta, mu" : "=r"(r) : "r"((long)vl));
    else if (!ta && ma)
        asm volatile("vsetvli %0, %1, e8, m1, tu, ma" : "=r"(r) : "r"((long)vl));
    else
        asm volatile("vsetvli %0, %1, e8, m1, tu, mu" : "=r"(r) : "r"((long)vl));
    return r;
}

/* ---------------- 载入操作数 + 执行 + 回读 ---------------- */
static void prep(int with_mask, unsigned maskbits)
{
    vset_e8(VL, 1, 1);                    /* 用完整 vl 载入, 保证 16 个源字节都有定义 */
    asm volatile("vle8.v v1, (%0)" :: "r"(A) : "memory");
    asm volatile("vle8.v v2, (%0)" :: "r"(B) : "memory");
    asm volatile("vle8.v v3, (%0)" :: "r"(OLD) : "memory");   /* v3 = 旧 vd, 用于 undisturbed 检查 */
    asm volatile("vmv.v.x v0, %0" :: "r"((long)maskbits) : "memory");
    asm volatile("" ::: "memory");
}

static void exec_vdot(int vl, int vm, int ta, int ma)
{
    vset_e8(vl, ta, ma);                  /* 指令真正执行时的 vl / vta / vma */
    if (vm)
        asm volatile(".insn 0x8220818b"); /* vdot.vv v3, v2, v1, vm=1 */
    else
        asm volatile(".insn 0x8020818b"); /* vdot.vv v3, v2, v1, vm=0 (v0 掩码) */
    vset_e8(VL, 1, 1);                    /* 恢复 vl 以便整寄存器回读 */
    asm volatile("vse8.v v3, (%0)" :: "r"(OUT) : "memory");
    asm volatile("" ::: "memory");
}

/* ---------------- 单个用例: 逐 32 位元素比对 ---------------- */
static void check_case(const char *name, int vl, int vm, int ta, int ma, unsigned maskbits)
{
    cur_case = name;
    prep(vm == 0, maskbits);
    exec_vdot(vl, vm, ta, ma);

    int ngroups = (vl + 3) / 4;
    for (int i = 0; i < 4; i++) {
        int32_t exp;
        if (vl == 0) {
            exp = rd32(OLD, i);                  /* vstart>=vl, vd 完全不变 */
        } else if (4 * i >= vl) {
            exp = ta ? (int32_t)-1 : rd32(OLD, i);         /* tail */
        } else if (!vm && ((maskbits >> i) & 1) == 0) {
            exp = ma ? (int32_t)-1 : rd32(OLD, i);         /* masked off */
        } else {
            exp = ref_dot(A, B, i);
        }
        int32_t act = rd32(OUT, i);
        checks++;
        if (act != exp) {
            fails++;
            if (fails <= 16)
                printf("  FAIL [%s] elem %d: act=%08x exp=%08x\n",
                       name, i, (unsigned)act, (unsigned)exp);
        }
    }
    (void)ngroups;
}

static void fill_pattern(uint8_t *p, int mode)
{
    for (int i = 0; i < VL; i++) {
        switch (mode) {
        case 0: p[i] = (uint8_t)(i + 1); break;                 /* 小正数 */
        case 1: p[i] = (uint8_t)(0x40 + (i & 0x3f)); break;     /* 较大正数 */
        case 2: p[i] = (uint8_t)(0x80 | (i & 0x7f)); break;     /* -128..-1 */
        case 3: p[i] = 0x80; break;                             /* -128 */
        case 4: p[i] = 0xff; break;                             /* -1 */
        case 5: p[i] = 0x7f; break;                             /* 127 */
        case 6: p[i] = 0x00; break;                             /* 0 */
        default:
            {
                static const uint8_t pat[8] = {0x7f, 0x80, 0x00, 0xff, 0x01, 0xfe, 0x40, 0xc0};
                p[i] = pat[i & 7];
            }
        }
    }
}

static void set_old(uint32_t seed)
{
    for (int i = 0; i < VL; i++) OLD[i] = (uint8_t)(seed + i * 0x11);
    /* old vd 里放可识别的 32 位图案: 0xdeadbeef ^ seed */
    for (int i = 0; i < 4; i++) {
        uint32_t w = 0xdeadbeefu ^ (seed + i);
        OLD[4 * i + 0] = (uint8_t)(w);
        OLD[4 * i + 1] = (uint8_t)(w >> 8);
        OLD[4 * i + 2] = (uint8_t)(w >> 16);
        OLD[4 * i + 3] = (uint8_t)(w >> 24);
    }
}

int main(void)
{
    printf("=== vdot.vv test start ===\n");

    /* ---------------- 第一组: 正数 ---------------- */
    printf("[group 1] positive\n");
    fill_pattern(A, 0); fill_pattern(B, 1);
    set_old(1);
    check_case("pos vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);
    check_case("pos vl=16 vm=1 tu,mu", 16, 1, 0, 0, 0);
    check_case("pos vl=8  vm=1 ta,ma", 8, 1, 1, 1, 0);
    check_case("pos vl=4  vm=1 ta,ma", 4, 1, 1, 1, 0);
    check_case("pos vl=4  vm=1 tu,mu", 4, 1, 0, 0, 0);

    /* ---------------- 第二组: 负数 (含 -128 * -128) ---------------- */
    printf("[group 2] negative\n");
    fill_pattern(A, 2); fill_pattern(B, 2);       /* -128..-1 互为操作数 */
    set_old(2);
    check_case("neg vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);
    check_case("neg vl=16 vm=1 tu,mu", 16, 1, 0, 0, 0);
    check_case("neg vl=6  vm=1 ta,ma", 6, 1, 1, 1, 0);

    fill_pattern(A, 3); fill_pattern(B, 3);       /* -128 * -128 = 16384, 4 项 = 65536 */
    set_old(3);
    check_case("neg min*min vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);
    check_case("neg min*min vl=16 vm=1 tu,mu", 16, 1, 0, 0, 0);

    fill_pattern(A, 3); fill_pattern(B, 4);       /* -128 * -1 */
    set_old(4);
    check_case("neg min*-1 vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);

    /* ---------------- 第三组: 边界 ---------------- */
    printf("[group 3] boundary\n");
    fill_pattern(A, 7); fill_pattern(B, 7);       /* 0x7f/0x80/0x00/0xff 混合 */
    set_old(5);
    check_case("bnd vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);
    check_case("bnd vl=15 vm=1 ta,ma", 15, 1, 1, 1, 0);
    check_case("bnd vl=15 vm=1 tu,mu", 15, 1, 0, 0, 0);
    check_case("bnd vl=5  vm=1 ta,ma", 5, 1, 1, 1, 0);
    check_case("bnd vl=4  vm=1 ta,ma", 4, 1, 1, 1, 0);
    check_case("bnd vl=3  vm=1 ta,ma", 3, 1, 1, 1, 0);
    check_case("bnd vl=3  vm=1 tu,mu", 3, 1, 0, 0, 0);
    check_case("bnd vl=1  vm=1 ta,ma", 1, 1, 1, 1, 0);
    check_case("bnd vl=0  vm=1 ta,ma", 0, 1, 1, 1, 0);

    /* vm=0: 输出元素 i 用 v0[i]; maskbits 的顺序与输出元素编号一致 */
    check_case("bnd vl=16 vm=0 mask=1111 ma,ta", 16, 0, 1, 1, 0xf);
    check_case("bnd vl=16 vm=0 mask=1111 mu,tu", 16, 0, 0, 0, 0xf);
    check_case("bnd vl=16 vm=0 mask=0101 ma", 16, 0, 1, 1, 0x5);
    check_case("bnd vl=16 vm=0 mask=0101 mu", 16, 0, 0, 0, 0x5);
    check_case("bnd vl=16 vm=0 mask=1010 ma", 16, 0, 1, 1, 0xa);
    check_case("bnd vl=16 vm=0 mask=0000 ma", 16, 0, 1, 1, 0x0);
    check_case("bnd vl=16 vm=0 mask=0000 mu", 16, 0, 0, 0, 0x0);
    check_case("bnd vl=6  vm=0 mask=0110 ma", 6, 0, 1, 1, 0x6);
    check_case("bnd vl=6  vm=0 mask=0110 mu", 6, 0, 0, 0, 0x6);
    check_case("bnd vl=0  vm=0 mask=1111 ma", 0, 0, 1, 1, 0xf);

    /* avl 超过 VLMAX 时 vsetvli 的截断: e8/m1 的 VLMAX = 16
     *   avl=17 -> VLMAX < avl < 2*VLMAX -> vl = ceil(17/2) = 9
     *   avl=32 -> vl = VLMAX = 16
     * 也就是说 vdot 永远看不到 vl > 16, 实现里的 vl 上界检查只是防御性 guard。
     */
    {
        size_t v17 = vset_e8(17, 1, 1);
        size_t v32 = vset_e8(32, 1, 1);
        /* RVV v1.0/v1.1 6.3: VLMAX < AVL <= 2*VLMAX 时, 实现可在
         * [ceil(AVL/2), VLMAX] 区间内任取 vl (不是必须 ceil(AVL/2));
         * AVL >= 2*VLMAX 时 vl 必须 = VLMAX.
         * e8/m1 的 VLMAX = 16, 所以 avl=17 的合法结果是 9..16, 只做区间检查. */
        checks++;
        if (!(v17 >= 9 && v17 <= 16 && v32 == 16)) {
            fails++;
            printf("  FAIL [avl clamp] avl=17 -> vl=%d (spec range 9..16), avl=32 -> vl=%d (exp 16)\n",
                   (int)v17, (int)v32);
        } else {
            printf("  avl clamp ok: avl=17 -> vl=%d (spec range 9..16), avl=32 -> vl=%d (must be 16)\n",
                   (int)v17, (int)v32);
        }
        check_case("bnd vl=16 (vsetvli avl=32) vm=1 ta,ma", 16, 1, 1, 1, 0);
        check_case("bnd vl=9  (vsetvli avl=17) vm=1 ta,ma", 9, 1, 1, 1, 0);
    }

    fill_pattern(A, 6); fill_pattern(B, 6);       /* 全 0 */
    set_old(6);
    check_case("bnd zero vl=16 vm=1 ta,ma", 16, 1, 1, 1, 0);

    printf("=== vdot.vv result: %d checks, %d fails -> %s ===\n",
           checks, fails, fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
