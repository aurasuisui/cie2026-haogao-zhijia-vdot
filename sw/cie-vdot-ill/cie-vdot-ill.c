/* 阶段二：vdot.vv 的 illegal instruction 负向测试（自带 trap handler，干净验证）
 *
 * 规范: 仅当 vtype.vsew == e8 且 vtype.vlmul == m1 时 vdot.vv 合法.
 * 这里先故意用 e16/m1 执行 vdot.vv, 期望触发 illegal instruction 异常(cause = 2);
 * 用自建 M-mode trap handler 捕获: 记录 mcause, mepc += 4 跳过非法指令, mret 继续.
 * 然后再用合法的 e8/m1 执行一次 vdot.vv, 确认异常路径没有破坏正常功能.
 *
 * 期望输出: [ILLEGAL-TEST] PASS: mcause=2 caught, legal path OK
 */
#include <klib.h>
#include <stdint.h>

#define INS_VDOT_VM1 0x8220818b

volatile int got_illegal = 0;
volatile unsigned long trap_cause = 0;

/* M-mode trap handler: 只处理 mcause=2 (illegal instruction) */
__asm__(".option norvc\n"
        ".align 2\n"
        ".globl cie_trap_handler\n"
        "cie_trap_handler:\n"
        "  csrr t0, mcause\n"
        "  la   t1, trap_cause\n"
        "  sd   t0, 0(t1)\n"
        "  li   t2, 2\n"
        "  bne  t0, t2, 9f\n"
        "  li   t2, 1\n"
        "  la   t3, got_illegal\n"
        "  sw   t2, 0(t3)\n"
        "  csrr t0, mepc\n"
        "  addi t0, t0, 4\n"
        "  csrw mepc, t0\n"
        "9:\n"
        "  mret\n");

void cie_trap_handler(void);

static void install_trap_handler(void)
{
    asm volatile("csrw mtvec, %0" :: "r"((unsigned long)cie_trap_handler));
}

static uint8_t A[16] __attribute__((aligned(16)));
static uint8_t B[16] __attribute__((aligned(16)));
static uint8_t OUT[16] __attribute__((aligned(16)));

static int32_t sext8(uint8_t x) { return (int32_t)(int8_t)x; }

static __attribute__((noinline)) void do_vdot(void)
{
    /* t0..t3 会被 trap handler 改写, 声明 clobber 让编译器不要在其间保存活跃值 */
    asm volatile(".insn 0x8220818b" ::: "t0", "t1", "t2", "t3", "memory");
}

int main(void)
{
    install_trap_handler();

    /* ---- 1) 非法 vtype: e16/m1 下执行 vdot.vv ---- */
    size_t vl = 0;
    asm volatile("vsetvli %0, %1, e16, m1, ta, ma" : "=r"(vl) : "r"((long)8) : "memory");
    printf("[ILLEGAL-TEST] vtype=e16,m1 vl=%d -> executing vdot.vv v3,v1,v2 (expect illegal)\n",
           (int)vl);
    do_vdot();
    asm volatile("" ::: "memory");

    if (got_illegal == 1 && trap_cause == 2) {
        printf("[ILLEGAL-TEST] caught illegal instruction: mcause=%d, mepc advanced by 4\n",
               (int)trap_cause);
    } else {
        printf("[ILLEGAL-TEST] FAIL: no illegal trap (got_illegal=%d, mcause=%d)\n",
               got_illegal, (int)trap_cause);
        return 1;
    }

    /* ---- 2) 合法 vtype: e8/m1, 确认异常路径没破坏正常执行 ---- */
    for (int i = 0; i < 16; i++) {
        A[i] = (uint8_t)(i + 1);
        B[i] = (uint8_t)(2 * i + 1);
    }
    asm volatile("vsetvli %0, %1, e8, m1, ta, ma" : "=r"(vl) : "r"((long)4) : "memory");
    asm volatile("vle8.v v1, (%0)" :: "r"(A) : "memory");
    asm volatile("vle8.v v2, (%0)" :: "r"(B) : "memory");
    asm volatile("" ::: "memory");
    do_vdot();
    asm volatile("vsetvli %0, %1, e8, m1, ta, ma" : "=r"(vl) : "r"((long)16) : "memory");
    asm volatile("vse8.v v3, (%0)" :: "r"(OUT) : "memory");
    asm volatile("" ::: "memory");

    int32_t exp = 0, act;
    for (int k = 0; k < 4; k++)
        exp += sext8(A[k]) * sext8(B[k]);
    act = (int32_t)((uint32_t)OUT[0] | ((uint32_t)OUT[1] << 8)
                  | ((uint32_t)OUT[2] << 16) | ((uint32_t)OUT[3] << 24));
    if (act != exp) {
        printf("[ILLEGAL-TEST] FAIL: legal case elem0 act=%08x exp=%08x\n",
               (unsigned)act, (unsigned)exp);
        return 1;
    }

    printf("[ILLEGAL-TEST] legal case OK: elem0=%d\n", (int)act);
    printf("[ILLEGAL-TEST] PASS: mcause=2 caught, legal path OK\n");
    return 0;
}
