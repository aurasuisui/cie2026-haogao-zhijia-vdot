package xiangshan.backend.fu.vector

import chisel3._

/** vdot.vv datapath (CIE custom-0 instruction).
  *
  * Treats vs2 / vs1 as 16 packed signed int8 lanes and produces 4 signed int32:
  *   out(i) = sum_{k=0..3} sext8(vs2[8 * (4i + k) +: 8]) * sext8(vs1[8 * (4i + k) +: 8])
  *
  * The exact sum is bounded by [-65024, 65536], which always fits into a signed
  * 32-bit word, so the "32-bit two's complement wraparound" of the spec is just a
  * sign extension here. No saturation, vxsat is never set.
  *
  * Purely combinational: the caller is responsible for registering the result on
  * the same beat as the rest of the VialuFix out stage (latency = 1).
  */
class Vdot32 extends Module {
  val io = IO(new Bundle {
    val vs2 = Input(UInt(128.W))
    val vs1 = Input(UInt(128.W))
    /** element 0 is the least significant 32 bits of the assembled vector */
    val out = Output(Vec(4, SInt(32.W)))
  })

  for (i <- 0 until 4) {
    val prods = (0 until 4).map { k =>
      val idx = 4 * i + k
      val a = io.vs2(8 * idx + 7, 8 * idx).asSInt
      val b = io.vs1(8 * idx + 7, 8 * idx).asSInt
      a * b // SInt(16)
    }
    // the sum needs 18 bits; the left fold widens 16 -> 17 -> 18 -> 19, then pad(32)
    io.out(i) := prods.reduce(_ +& _).pad(32)
  }
}
