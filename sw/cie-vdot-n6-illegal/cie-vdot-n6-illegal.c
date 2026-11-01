/* n6: vdot.vv 前置条件 (illegal) 检查
 *
 * 语义: vdot.vv 仅当 vtype.vill=0, vsew=e8, vlmul=m1, vl<=16 时合法, 否则触发
 *       illegal instruction (mcause=2)。
 * 本程序安装最小 trap 处理程序 (记录 mcause, mepc+=4 跳过指令, mret),
 * 因此可以在一次运行里连续检查多个非法配置, 并验证随后合法配置仍然工作。
 */
#include <am.h>
#include <klib.h>

static volatile uint64_t t_cause;
static volatile uint64_t t_count;
static uint8_t out[16] __attribute__((aligned(16)));

extern void trap_entry(void);

/* 最小 trap 处理: 记录 mcause/count, 跳过当前 4 字节指令, 强制 MPP=M 后 mret */
__asm__(
  ".section .text.trap,\"ax\"\n"
  ".align 2\n"
  ".globl trap_entry\n"
  ".type trap_entry, @function\n"
  "trap_entry:\n"
  "  addi sp, sp, -32\n"
  "  sd t0, 0(sp)\n"
  "  sd t1, 8(sp)\n"
  "  sd t2, 16(sp)\n"
  "  csrr t0, mcause\n"
  "  la t1, t_cause\n"
  "  sd t0, 0(t1)\n"
  "  la t1, t_count\n"
  "  ld t2, 0(t1)\n"
  "  addi t2, t2, 1\n"
  "  sd t2, 0(t1)\n"
  "  csrr t0, mepc\n"
  "  addi t0, t0, 4\n"
  "  csrw mepc, t0\n"
  "  li t0, 0x1800\n"
  "  csrs mstatus, t0\n"
  "  ld t0, 0(sp)\n"
  "  ld t1, 8(sp)\n"
  "  ld t2, 16(sp)\n"
  "  addi sp, sp, 32\n"
  "  mret\n"
  ".previous\n");

static int g_checks;
static int g_fails;

static void expect_illegal(const char *tag, int kind, size_t avl)
{
  uint64_t before = t_count;
  switch (kind) {
    case 0: asm volatile("vsetvli x0, %0, e16, m1, ta, ma" : : "r"((long)avl)); break;
    case 1: asm volatile("vsetvli x0, %0, e8, m2, ta, ma" : : "r"((long)avl)); break;
    default: asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)avl)); break;
  }
  asm volatile(".insn 0x8220818b" ::: "memory");     /* vdot.vv v3, v2, v1 (vm=1) */
  int traps = (int)(t_count - before);
  int ok = (traps == 1) && (t_cause == 2);
  g_checks++;
  if (!ok) g_fails++;
  printf("%s %-22s kind=%d avl=%2d traps=%d mcause=%d (expect 1 次, mcause=2)\n",
         ok ? "PASS" : "FAIL", tag, kind, (int)avl, traps, (int)t_cause);
}

static void expect_legal(const char *tag, size_t avl)
{
  uint64_t before = t_count;
  asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)avl));
  asm volatile(".insn 0x8220818b" ::: "memory");
  int traps = (int)(t_count - before);
  g_checks++;
  if (traps) g_fails++;
  printf("%s %-22s avl=%2d traps=%d (expect 0)\n", traps ? "FAIL" : "PASS", tag, (int)avl, traps);
}

int main(void)
{
  printf("=== n6-illegal: 前置条件 vsew=e8 / vlmul=m1 / vl<=16 / vill=0 ===\n");
  asm volatile("la t0, trap_entry\n\tcsrw mtvec, t0" : : : "t0");

  asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)16));
  asm volatile("vmv.v.i v1, 1");        /* vs2 = 每个 e8 = 1 */
  asm volatile("vmv.v.i v2, 1");        /* vs1 = 每个 e8 = 1 */
  asm volatile("vmv.v.i v3, 0");        /* vd  = 0 */

  expect_illegal("vsew=e16", 0, 8);     /* SEW 不对 -> illegal */
  expect_illegal("vlmul=m2", 1, 8);     /* LMUL 不对 -> illegal */
  expect_illegal("vsew=e16 (again)", 0, 4);
  expect_legal("e8,m1 vl=16", 16);      /* 合法 */
  expect_legal("e8,m1 vl=0", 0);        /* 合法 (vl=0 不写回, 但不是 illegal) */

  /* 合法执行的结果仍然是 4*1*1 = 4 */
  asm volatile("vsetvli x0, %0, e8, m1, ta, ma" : : "r"((long)16));
  asm volatile(".insn 0x8220818b");    /* GNU 汇编器不认识 vdot.vv 助记符, 只能写编码 */
  asm volatile("vse8.v v3, (%0)" : : "r"(out) : "memory");
  int ok = 1;
  for (int i = 0; i < 4; i++) {                  /* e32 元素 i = 0x00000004 */
    uint32_t v = (uint32_t)out[4 * i] | ((uint32_t)out[4 * i + 1] << 8) |
                 ((uint32_t)out[4 * i + 2] << 16) | ((uint32_t)out[4 * i + 3] << 24);
    if (v != 4) ok = 0;
  }
  g_checks++;
  if (!ok) g_fails++;
  printf("%s legal vdot.vv (e8,m1,vl=16) vd.e32 = %08x %08x %08x %08x (expect 00000004 x4)\n",
         ok ? "PASS" : "FAIL",
         (unsigned)(out[0] | (out[1] << 8) | (out[2] << 16) | (out[3] << 24)),
         (unsigned)(out[4] | (out[5] << 8) | (out[6] << 16) | (out[7] << 24)),
         (unsigned)(out[8] | (out[9] << 8) | (out[10] << 16) | (out[11] << 24)),
         (unsigned)(out[12] | (out[13] << 8) | (out[14] << 16) | (out[15] << 24)));

  printf("=== n6-illegal: %d checks, %d fails -> %s ===\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
  return g_fails ? 1 : 0;
}
