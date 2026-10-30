/* n3: vdot.vv vl 扫描 (0..16) x vta/vma
 *
 * 被测指令: vdot.vv vd, vs2, vs1
 *   编码: opcode=0x0B (custom-0), funct6=100000, funct3=000, vm=funct7[0]
 *   机器码: 0x8220818b (vm=1) / 0x8020818b (vm=0), rd=v3, rs1=v2, rs2=v1
 *   语义: vd.e32[i] = sum_{k=0..3} sext8(vs2[4i+k]) * sext8(vs1[4i+k]), 32 位回绕, 无饱和/无 vxsat
 *   前置: vsew=e8, vlmul=m1, vl<=16, vill=0
 *   mask: 元素 i (e32 下标) 由 v0[i] 控制; vstart 按源字节计, i*4 < vstart 的元素保持旧值
 *
 * 本文件内含 C 标量黄金模型 (golden) 与 NEMU 执行结果 (ACT) 的逐元素对拍。
 */
#include <am.h>
#include <klib.h>

#define VLMAX 16                 /* e8,m1 的 VLMAX */
#define NBYTE 64                 /* 同时检查 v3 前 4 个 e32 元素之外的字节未被改写 */
static uint8_t A[NBYTE]   __attribute__((aligned(16)));   /* vs1 源 (v2) */
static uint8_t B[NBYTE]   __attribute__((aligned(16)));   /* vs2 源 (v1) */
static uint8_t OLD[NBYTE] __attribute__((aligned(16)));   /* vd 旧值 */
static uint8_t ACT[NBYTE] __attribute__((aligned(16)));   /* NEMU 执行后回读的 vd */
static uint8_t REF[NBYTE] __attribute__((aligned(16)));   /* 黄金模型结果 */
static uint8_t MSK[NBYTE] __attribute__((aligned(16)));   /* v0 的 mask 位图 (0xff/0x00) */

static int g_checks;
static int g_fails;
static int g_mask = 0xf;

static void __attribute__((unused)) fill_all(uint8_t *p, int v)
{
  for (int i = 0; i < NBYTE; i++) p[i] = (uint8_t)v;
}

static void fill_pat(int mode)
{
  for (int i = 0; i < NBYTE; i++) {
    switch (mode) {
      case 0:  A[i] = (uint8_t)(i + 1);                   B[i] = (uint8_t)(17 + i); break;
      case 1:  A[i] = (uint8_t)(0x40 + (i & 0x1f));        B[i] = (uint8_t)(0x30 + (i & 0x0f)); break;
      case 2:  A[i] = (uint8_t)(-1 - (i % 9));             B[i] = (uint8_t)(1 + i); break;
      case 3:  A[i] = (uint8_t)0x80;                       B[i] = (uint8_t)0x80; break;
      case 4:  A[i] = (uint8_t)0x7f;                       B[i] = (uint8_t)0x7f; break;
      case 5:  A[i] = (uint8_t)0x80;                       B[i] = (uint8_t)0x7f; break;
      case 6:  A[i] = (uint8_t)(i * 37 - 100);             B[i] = (uint8_t)(i * 11 - 50); break;
      default: A[i] = 0;                                   B[i] = 0; break;
    }
  }
}

static inline uint32_t rd32(const uint8_t *p, int i)
{
  return (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
         ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
}

static inline void wr32(uint8_t *p, int i, uint32_t v)
{
  p[4 * i] = (uint8_t)v;
  p[4 * i + 1] = (uint8_t)(v >> 8);
  p[4 * i + 2] = (uint8_t)(v >> 16);
  p[4 * i + 3] = (uint8_t)(v >> 24);
}

static inline int32_t s8(const uint8_t *p, int k) { return (int32_t)(int8_t)p[k]; }

/* ---------------- C 标量黄金模型 (架构语义) ---------------- */
static void golden(size_t vl, int vm, int vta, int vma, uint32_t vstart)
{
  for (int i = 0; i < NBYTE; i++) REF[i] = OLD[i];      /* 未写回的元素保持 vd 旧值 */
  if (vstart >= (uint32_t)vl) return;                    /* vstart>=vl: 整条指令不写回 */
  uint32_t vlout = ((uint32_t)vl + 3) / 4;               /* 输出元素数 */
  for (uint32_t i = 0; i < 4; i++) {
    if (i * 4 < vstart) continue;                        /* 该元素的源字节已被处理 → 旧值 */
    if (i >= vlout) {                                    /* tail */
      if (vta) wr32(REF, (int)i, 0xffffffffu);
      continue;
    }
    if (!vm && ((((uint32_t)g_mask >> i) & 1u) == 0)) {  /* masked-off */
      if (vma) wr32(REF, (int)i, 0xffffffffu);
      continue;
    }
    int32_t acc = 0;
    for (int k = 0; k < 4; k++) acc += s8(A, (int)(i * 4 + k)) * s8(B, (int)(i * 4 + k));
    wr32(REF, (int)i, (uint32_t)acc);                    /* 32 位二进制回绕 */
  }
}

/* ---------------- 在 NEMU 上执行一次 vdot.vv ---------------- */
static void run_nemu(size_t vl, int vm, int vta, int vma, uint32_t vstart)
{
  for (int i = 0; i < NBYTE; i++) {
    OLD[i] = (uint8_t)(0x91 + i * 7);
    ACT[i] = OLD[i];
    MSK[i] = 0;
  }
  /* mask 寄存器按元素编号位打包: bit i 控制输出元素 i (NEMU: vreg_impl.c:63-68 get_mask) */
  MSK[0] = (uint8_t)(g_mask & 0xff);
  MSK[1] = (uint8_t)((g_mask >> 8) & 0xff);

  asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)VLMAX));   /* 满 VLMAX 装载 */
  asm volatile("vle8.v v1, (%0)" : : "r"(B) : "memory");                 /* vs2 */
  asm volatile("vle8.v v2, (%0)" : : "r"(A) : "memory");                 /* vs1 */
  asm volatile("vle8.v v0, (%0)" : : "r"(MSK) : "memory");               /* mask 位图 */
  asm volatile("vle8.v v3, (%0)" : : "r"(ACT) : "memory");               /* vd 旧值 */

  switch ((vta << 1) | vma) {                                           /* 设置 vta/vma 与 vl */
    case 0: asm volatile("vsetvli x0, %0, e8, m1, tu, mu" : : "r"((long)vl)); break;
    case 1: asm volatile("vsetvli x0, %0, e8, m1, tu, ma" : : "r"((long)vl)); break;
    case 2: asm volatile("vsetvli x0, %0, e8, m1, ta, mu" : : "r"((long)vl)); break;
    default: asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)vl)); break;
  }
  asm volatile("csrw vstart, %0" : : "r"((long)vstart));                /* vstart 以源字节计 */
  if (vm) asm volatile(".insn 0x8220818b" ::: "memory");                /* vdot.vv v3,v1,v2 vm=1 */
  else    asm volatile(".insn 0x8020818b" ::: "memory");                /* vdot.vv v3,v1,v2 vm=0 */
  asm volatile("csrw vstart, x0");

  asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)VLMAX));
  asm volatile("vse8.v v3, (%0)" : : "r"(ACT) : "memory");
}

static void check_case(const char *tag, size_t vl, int vm, int vta, int vma, uint32_t vstart)
{
  run_nemu(vl, vm, vta, vma, vstart);
  golden(vl, vm, vta, vma, vstart);

  int bad = 0;
  g_checks += 4;
  for (int i = 0; i < 4; i++) {
    uint32_t a = rd32(ACT, i), e = rd32(REF, i);
    if (a != e) { bad++; printf("  MISMATCH el%d act=%08x exp=%08x\n", i, a, e); }
  }
  g_checks += NBYTE - 16;
  for (int i = 16; i < NBYTE; i++) {
    if (ACT[i] != OLD[i]) { bad++; printf("  CLOBBER byte%d act=%02x exp=%02x\n", i, ACT[i], OLD[i]); }
  }
  g_fails += bad;

  printf("%s %-22s vl=%2d vm=%d ta=%d ma=%d vstart=%2d mask=%04x act=[%08x %08x %08x %08x]\n",
         bad ? "FAIL" : "PASS", tag, (int)vl, vm, vta, vma, (int)vstart, g_mask,
         rd32(ACT, 0), rd32(ACT, 1), rd32(ACT, 2), rd32(ACT, 3));
}

static int report(const char *name)
{
  printf("=== %s: %d checks, %d fails -> %s ===\n", name, g_checks, g_fails, g_fails ? "FAIL" : "PASS");
  return g_fails ? 1 : 0;
}

int main(void)
{
  printf("=== n3-vl : vl 扫描 ===\n");
  static const int vls[] = { 16, 15, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
  fill_pat(6);
  for (unsigned j = 0; j < sizeof(vls) / sizeof(vls[0]); j++) {
    check_case("vl sweep ta,ma", (size_t)vls[j], 1, 1, 1, 0);
  }
  for (unsigned j = 0; j < sizeof(vls) / sizeof(vls[0]); j++) {
    check_case("vl sweep tu,mu", (size_t)vls[j], 1, 0, 0, 0);
  }
  for (unsigned j = 0; j < sizeof(vls) / sizeof(vls[0]); j++) {
    check_case("vl sweep ta,mu", (size_t)vls[j], 1, 1, 0, 0);
  }
  return report("n3-vl");
}
