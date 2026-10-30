# sw/ — 测试程序与基线程序

全部程序都是 **bare-metal AM 应用**（nexus-am），跑在香山 emu（Verilator）或 NEMU 上，不依赖 Linux 用户态。

```bash
export AM_HOME=/home/aura/xs-env/nexus-am       # 或你自己的 xs-env/nexus-am
cp -r sw/cie-vdot-*  $AM_HOME/apps/
make -C $AM_HOME/apps/cie-vdot ARCH=riscv64-xs  # 产物 build/cie-vdot-riscv64-xs.bin
```

## 程序清单

| 目录 | 用途 | 运行方式 |
|---|---|---|
| `cie-vdot-baseline/` | 用**标准 RVV 指令**实现等价语义的三个基线变体（`gat` 12 条 / `ngt` 10 条 / `acc` 12 条），带功能对拍与 `mcycle` 计时 | `emu -i .../cie-vdot-baseline-riscv64-xs.bin --no-diff -C 400000` |
| `cie-vdot-perf/` | **统一测量框架**：同一二进制内依次测三个基线与 `vdot`，同一套 `bench_ser`/`bench_thru`/`bench_null`，内嵌 `vdot` 功能自检 | 同上（见文末输出样例） |
| `cie-vdot/` | 阶段二主测试：正数 / 负数 / 边界三组 + `vstart`/`vmask` 组合，133 checks | `emu -i .../cie-vdot-riscv64-xs.bin --no-diff -C 40000` |
| `cie-vdot-ill/` | 负向测试：`vsew=e16` 必须触发 illegal instruction（自带 M-mode trap handler） | 同上 |
| `cie-vdot-n1-pos/` … `n6-illegal/` | 六个分项测试（正数、负数、vl 扫描、mask/tail 矩阵、vstart 重启、非法指令），供 difftest 分组比对 | `emu -i ...bin -C 1500000 --diff=$NEMU_HOME/build/riscv64-nemu-interpreter-so` |

## 编译期开关（`cie-vdot-baseline` / `cie-vdot-perf` 的 Makefile）

| 宏 | 默认 | 说明 |
|---|---|---|
| `USE_VWADD_WV` | 0 | 1 = 启用 `acc` 变体。**本仓库默认 0**：该变体在本机 emu 上会挂死（见 `docs/03` §6.4），挂死与 vdot 本身的正确性无关 |
| `BENCH_THRU` | 1 | 1 = 同时跑「4 条互不依赖的链」的吞吐口径 |
| `VDOT_REF` | 0 | 1 = 把 vdot 也纳入功能对拍（需要含 vdot 的 RTL/NEMU） |
| `DUMP` | 0 | 1 = 打印前 4 个用例的输入/输出，便于人工核对 |

## `cie-vdot-perf` 的输出样例

```
[gat] 40/40
[ngt] 40/40
[B] N=100 nop=457 gat=15659,40569 ngt=13866,51878
[vdot] 40/40
[BV] N=100 vdot=5923,2970
Core 0: HIT GOOD TRAP at pc = 0x80000876
```

- `[B]` / `[BV]` 行里每个变体是「**串行总周期, 吞吐总周期**」；串行每轮依赖上一轮结果，吞吐每轮 4 条独立链。
- 换算：串行延迟 = `(ser − nop) / N`，吞吐 = `(thru − nop) / (4N)`，单位 cycle/点积（4 个 int8 乘加 → 1 个 int32）。
- `nop` 是同骨架空循环，必须扣除；N=100 时它占 vdot 吞吐口径的 73%。

实测结果与解释见 `docs/03-协同仿真与性能分析.md` §5.3–§5.4。
