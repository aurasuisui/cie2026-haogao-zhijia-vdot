# vdot.vv — 面向边缘 AI 推理的 RISC-V 自定义向量点积指令（CIE 2026 · 香山社区赛道）

> 参赛团队：恶搞之家　|　赛道：CIE-香山社区 RISC-V 应用创新赛道
> 赛题：面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计 —— 基于「香山」昆明湖 V2 处理器架构
> 命题方：北京开源芯片研究院（BOSC）

本仓库是参赛作品的**唯一代码与文档入口**，覆盖赛程要求的三个阶段：

| 阶段 | 交付物 | 位置 |
|---|---|---|
| 阶段一 · 环境部署与向量指令波形分析（15 分） | 环境部署日志、`hello xiangshan` 输出、`vadd.vv` 逐拍波形分析 | [docs/01-环境部署与指令波形分析.md](docs/01-环境部署与指令波形分析.md) |
| 阶段二 · 自定义 `vdot.vv` 指令设计（20 分） | 设计文档、RTL 实现、正/负/边界三组测试 | [docs/02-vdot设计与实现.md](docs/02-vdot设计与实现.md)、[rtl/](rtl/) |
| 阶段三 · NEMU 与软硬件协同仿真（25 分） | NEMU 功能模型、NEMU-RTL 协同仿真、基线对比与加速比分析 | [docs/03-协同仿真与性能分析.md](docs/03-协同仿真与性能分析.md)、[nemu/](nemu/)、[sw/](sw/) |

## 指令速览

    vdot.vv vd, vs2, vs1

- **语义**：把 `vs2`/`vs1` 各看作 16 个 `int8`，输出 4 个 `int32`：
  `vd.e32[i] = Σ_{k=0..3} sext8(vs2[4i+k]) × sext8(vs1[4i+k])`，32 位补码回绕、不饱和、不置 `vxsat`。
- **编码**：custom-0（opcode `0x0B`，即 RISC-V 保留给厂商自定义的编码空间）＋ funct6 `100000` ＋ funct3 `000`。
  机器码：`.insn 0x8220818b`（vm=1）、`.insn 0x8020818b`（vm=0）。
- **前置条件**：`vtype.vill=0`、`vsew=e8`、`vlmul=m1`、`vl ≤ 16`；否则触发 illegal instruction。
- **为什么是 custom-0**：不占用 RVV/Zv* 的 funct6 槽位，避免与后续标准扩展冲突（详见设计文档 §2.4）。

## 复现摘要

    # 1) 环境（见 docs/01）
    git clone https://github.com/OpenXiangShan/xs-env && cd xs-env
    sudo -s ./setup-tools.sh && source setup.sh
    source ./env.sh

    # 2) 打上本仓库的 RTL 改动（两个仓库分别打）
    cd $NOOP_HOME && git apply /path/to/rtl/xiangshan-vdot.patch
    cd $NOOP_HOME/yunsuan && git apply /path/to/rtl/yunsuan-vdot.patch
    cd $NOOP_HOME && git submodule status yunsuan    # 确认子模块指针已更新

    # 3) 构建仿真器，跑三组功能测试与非法指令测试
    make emu -j16
    ./build/emu -i $AM_HOME/apps/cie-vdot/build/cie-vdot-riscv64-xs.bin
    ./build/emu -i $AM_HOME/apps/cie-vdot-ill/build/cie-vdot-ill-riscv64-xs.bin

## 目录

    docs/      三阶段文档（Markdown，含图表与逐条源码依据）；docs/pdf/ 为对应 PDF（官网提交版本）
    rtl/       XiangShan 侧改动（xiangshan-vdot.patch + yunsuan-vdot.patch + 新增模块 Vdot32.scala）
    nemu/      NEMU 侧 vdot.vv 实现（patch）
    sw/        测试程序、黄金模型、基线对比程序（cie-vdot / cie-vdot-ill / n1..n6 / cie-vdot-perf）
    scripts/   一键复现脚本

## 许可证

本仓库原创部分（文档、测试程序、`Vdot32.scala`、NEMU 实现）采用 **Mulan PSL v2**；修改自香山（OpenXiangShan/XiangShan）与云算（YunSuan）的部分沿用其 Apache-2.0 许可。
