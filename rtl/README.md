# rtl/ —— vdot.vv 的 RTL 改动

本目录是「香山昆明湖 V2 + 云算（YunSuan）向量 ALU」上新增自定义指令 `vdot.vv` 的全部源码改动，以补丁 + 新增文件的形式给出，**不改动任何已有标准指令的行为**。

上位仓库版本锚点（复现时必须对齐）：

| 仓库 | 版本 |
|---|---|
| OpenXiangShan/XiangShan | 分支 `kunminghu-v2`，commit `7b16188` |
| 子模块 `yunsuan`（YunSuan） | commit `9559211`（2025-09-24，chisel 6.7.0） |

## 文件

| 文件 | 作用 |
|---|---|
| `xiangshan-vdot.patch` | 对 `XiangShan` 的两处补丁：`backend/decode/VecDecoder.scala`（译码表项）、`backend/fu/wrapper/VIAluFix.scala`（数据通路接入 + 掩码/尾元素/前置条件） |
| `yunsuan-vdot.patch` | 对子模块 `yunsuan` 的两处补丁：`encoding/Opcode/VialuOpcode.scala`（新 opcode 33）、`package.scala`（`VialuFixType.vdot_vv = 0x061`） |
| `new/src/main/scala/xiangshan/backend/fu/vector/Vdot32.scala` | 新增模块：4 路 `sext8 × sext8 → 32 位` 点积树 |

## 应用方式

```bash
cd xs-env
source ./env.sh

cd XiangShan
git checkout 7b16188            # kunminghu-v2 的锚定 commit
git submodule update --init yunsuan
git -C yunsuan checkout 9559211

git apply /path/to/rtl/xiangshan-vdot.patch
git -C yunsuan apply /path/to/rtl/yunsuan-vdot.patch
cp /path/to/rtl/new/src/main/scala/xiangshan/backend/fu/vector/Vdot32.scala \
   src/main/scala/xiangshan/backend/fu/vector/Vdot32.scala

make emu -j16                   # 首次约 60–90 分钟
```

## 编码（custom-0）

```
vdot.vv vd, vs2, vs1        # vd.e32[i] = Σ_{k=0..3} sext8(vs2[4i+k]) * sext8(vs1[4i+k])
```

`enc = (funct7 << 25) | (vs2 << 20) | (vs1 << 15) | (funct3 << 12) | (vd << 7) | 0x0B`，`funct7 = 100000<vm>`，`funct3 = 000`。

| 形式 | 机器码（`vdot.vv v3, v2, v1`） |
|---|---|
| `vm = 1`（不掩码） | `0x8220818b` |
| `vm = 0`（用 v0 掩码） | `0x8020818b` |

汇编写法（GNU as 无法用 `.insn r` 生成任何带 `vN` 操作数的自定义指令，必须整条写死）：

```asm
.insn 0x8220818b        # 或 .word 0x8220818b
```

## 前置条件

`vill = 0`、`vsew = e8`、`vlmul = m1`、`vl ≤ 16`；任一不满足则触发 `illegalInstr` 异常。
`vl` 按**源字节**计数，输出元素个数为 `ceil(vl/4)`；`vm = 0` 时第 `i` 个输出元素由 `v0[i]` 控制。
