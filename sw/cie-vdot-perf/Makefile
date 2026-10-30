NAME = cie-vdot-perf
SRCS = cie-vdot-perf.c

# 需要 V 扩展：vsetvli/vle8.v/vwmul.vv/vwadd.vv/vwadd.wv/vslidedown.vi/
#             vadd.vv/vrgather.vv/vid.v/vsll.vi/vse32.v + .insn 形式的 vdot.vv
MARCH := rv64gcv_zba_zbb_zbc_zbs_zbkb_zbkc_zbkx_zknd_zkne_zknh_zkr_zksed_zksh_zkt

# USE_VWADD_WV=1 : 启用 "vwadd.wv 逐列累加" 版本（emu 可用；NEMU 未实现该指令，用 0）
# VDOT_REF=1     : 额外跑自定义 vdot.vv 做三方对比（需要支持 vdot 的模型）
# DUMP=1         : 打印每个用例的结果数组（NEMU 用；emu 上 printf 极慢，别开）
USE_VWADD_WV ?= 1
BENCH_THRU   ?= 1
VDOT_REF     ?= 1
DUMP         ?= 0
CFLAGS += -DUSE_VWADD_WV=$(USE_VWADD_WV) -DVDOT_REF=$(VDOT_REF) -DDUMP=$(DUMP) \
          -DBENCH_THRU=$(BENCH_THRU)

include $(AM_HOME)/Makefile.app
