//===-- RISCVSchedDandelion.td - Dandelion Scheduling -----*- tablegen -*-===//
//
// This file defines the itinerary-based scheduling model for the "Dandelion"
// RISC-V processor.
//
// Architecture reference: doc/arch.md
//
// Pipeline topology (8 parallel in-order pipelines, 6 stages: IF ID EX1 EX2 EX3 WB):
//
//  Slot │ Pipeline type    │ Part 1 (EX1)  │ Part 2 (EX1-EX3) │ Forward at
//  ─────┼──────────────────┼───────────────┼──────────────────┼───────────
//   0   │ FDivFPUPipeline  │ FDiv          │ FPU              │ WB  (both)
//   1   │ ALUFPUPipeline   │ ALU           │ FPU (+FPToInt)   │ EX2+EX3 / WB
//   2   │ ALUFPUPipeline   │ ALU           │ FPU (+IntToFP)   │ EX2+EX3 / WB
//   3   │ ALUiMDPipeline   │ ALU           │ iMul/Div         │ EX2+EX3 / WB
//   4   │ ALUiMDPipeline   │ ALU           │ iMul/Div         │ EX2+EX3 / WB
//   5   │ ALULSUPipeline   │ ALU           │ LSU              │ EX2+EX3 / WB
//   6   │ ALULSUPipeline   │ ALU           │ LSU              │ EX2+EX3 / WB
//   7   │ ALUBranchPipeline│ ALU + Branch  │ –                │ EX2+EX3
//
// Forwarding (bypass) summary from arch.md §Pipeline forwarding table:
//
//  Pipeline │ EX2       │ EX3       │ WB
//  ─────────┼───────────┼───────────┼────────────
//   0       │ No        │ No        │ FPU + FDiv
//   1-2     │ ALU       │ ALU       │ ALU + FPU
//   3-4     │ ALU       │ ALU       │ ALU + iMDU
//   5-6     │ ALU       │ ALU       │ ALU + LSU
//   7       │ ALU       │ ALU       │ ALU
//
//  Three bypass networks are required:
//    Dandelion_EX2_Bypass – ALU / Branch results first available at EX2
//    Dandelion_EX3_Bypass – same ALU result still valid at EX3
//    Dandelion_WB_Bypass  – MUL / DIV / FPU / FDiv / Load results at WB
//
// How integer source operand bypasses are encoded (LLVM constraint):
//   LLVM's hasPipelineForwarding() matches DefBypass==UseBypass by operand
//   index (one slot per operand).  To model that an integer source can be
//   read from EITHER EX2 OR EX3, each integer source operand is listed TWICE
//   in the operandcycles / bypasses lists with the same latency value but
//   different bypass IDs.  The scheduler scans all slots and returns true on
//   the first match, so either bypass path is accepted.
//
//   Example for a 2-source integer instruction (rs1, rs2):
//     operandcycles: [DefCycle, 1, 1, 1, 1]
//     bypasses:      [DefBP,  EX2_BP, EX3_BP, EX2_BP, EX3_BP]
//                     ^^^^^^  ^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^
//                     def     rs1 (two paths)  rs2 (two paths)
//
// InstrItinData bypass list meaning (4th argument):
//   [DefBypass, UseBypass_rs1_via_EX2, UseBypass_rs1_via_EX3,
//               UseBypass_rs2_via_EX2, UseBypass_rs2_via_EX3, ...]
//
// The shared FuncUnit and IIC_* definitions are in RISCVScheduleVLIW.td.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Bypass network definitions
//
//  Dandelion_EX2_Bypass
//    ALU / Branch results first placed on the forwarding bus at EX2.
//    Pipelines 1-7.  A consumer issuing 1 cycle after the producer reads
//    the value from the EX2 forwarding register at the start of its EX1.
//
//  Dandelion_EX3_Bypass
//    The same ALU / Branch result is retained in the EX3 pipeline register
//    one cycle later (arch.md EX3 column = ALU for pipelines 1-7).
//    A consumer issuing 2 cycles after the producer reads it from EX3.
//    Used exclusively on the UseBypass side alongside EX2_Bypass so that
//    both forwarding windows are explicitly declared.
//
//  Dandelion_WB_Bypass
//    Slow-unit (MUL, DIV, FPU, FDiv, Load) results forwarded only at WB.
//    Pipeline 0 exposes only this path; pipelines 1-6 also expose it for
//    their respective slow units.
//===----------------------------------------------------------------------===//

def Dandelion_EX2_Bypass : Bypass;
def Dandelion_EX3_Bypass : Bypass;
def Dandelion_WB_Bypass  : Bypass;

//===----------------------------------------------------------------------===//
// ALU instructions  (pipelines 1-7, EX1 only)
//
//  DefCycle = 2 (EX2).  Source operands needed at EX1 (cycle 1).
//  DefBypass = EX2_Bypass : result first appears on the EX2 forwarding bus.
//  Each integer source operand is listed twice:
//    - once with EX2_Bypass (consumer issues 1 cycle after producer)
//    - once with EX3_Bypass (consumer issues 2 cycles after producer)
//  operandcycles: [2,  1,       1,        1,       1      ]
//  bypasses:      [EX2, EX2_rs1, EX3_rs1,  EX2_rs2, EX3_rs2]
//===----------------------------------------------------------------------===//

def DandelionIIC_ALU : InstrItinData<
  IIC_ALU,
  [InstrStage<1, [RISCV_SLOT1, RISCV_SLOT2, RISCV_SLOT3,
                  RISCV_SLOT4, RISCV_SLOT5, RISCV_SLOT6, RISCV_SLOT7]>],
  [2, 1, 1, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

def DandelionIIC_ALU32 : InstrItinData<
  IIC_ALU32,
  [InstrStage<1, [RISCV_SLOT1, RISCV_SLOT2, RISCV_SLOT3,
                  RISCV_SLOT4, RISCV_SLOT5, RISCV_SLOT6, RISCV_SLOT7]>],
  [2, 1, 1, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;

def DandelionIIC_Shift : InstrItinData<
  IIC_Shift,
  [InstrStage<1, [RISCV_SLOT1, RISCV_SLOT2, RISCV_SLOT3,
                  RISCV_SLOT4, RISCV_SLOT5, RISCV_SLOT6, RISCV_SLOT7]>],
  [2, 1, 1, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;

//===----------------------------------------------------------------------===//
// Branch / Jump  (pipeline 7, EX1 only, forwarded at EX2+EX3)
//
//  arch.md §ALUBranchPipeline: EX2 and EX3 both forward ALU/Branch results.
//  Each source operand listed twice (EX2 + EX3 paths).
//===----------------------------------------------------------------------===//

def DandelionIIC_Branch : InstrItinData<
  IIC_Branch,
  [InstrStage<1, [RISCV_SLOT7]>],
  [2, 1, 1, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

// JAL/JALR: single integer source (rs1 base address).
def DandelionIIC_Jump : InstrItinData<
  IIC_Jump,
  [InstrStage<1, [RISCV_SLOT7]>],
  [2, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs1: EX2 or EX3

//===----------------------------------------------------------------------===//
// Integer Multiply  (pipelines 3-4, EX1-EX3, result at WB)
//
//  arch.md §Multiply: result forwarded at WB only.
//  Two integer source operands, each available via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_MUL : InstrItinData<
  IIC_MUL,
  [InstrStage<3, [RISCV_SLOT3, RISCV_SLOT4]>],
  [4, 1, 1, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

//===----------------------------------------------------------------------===//
// Integer Divide  (pipelines 3-4, SRT2 iterative, result at WB)
//
//  arch.md §SRT2: result forwarded at WB only.
//  Two integer source operands, each available via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_DIV : InstrItinData<
  IIC_DIV,
  [InstrStage<20, [RISCV_SLOT3, RISCV_SLOT4]>],
  [21, 1, 1, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

//===----------------------------------------------------------------------===//
// FPU – general scalar FP  (pipelines 0-2, EX1-EX3, result at WB)
//
//  arch.md §FPU: result forwarded at WB only.
//  FP source operands travel the WB path only (no EX2/EX3 for FP).
//===----------------------------------------------------------------------===//

def DandelionIIC_FPU : InstrItinData<
  IIC_FPU,
  [InstrStage<3, [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2]>],
  [4, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_WB_Bypass,   // fp src1
   Dandelion_WB_Bypass]>; // fp src2

//===----------------------------------------------------------------------===//
// FP→Int  FCVT.W.S / FCVT.WU.S  (pipeline 1 only, result at WB)
//
//  Single FP source → WB bypass only.
//===----------------------------------------------------------------------===//

def DandelionIIC_FPToInt : InstrItinData<
  IIC_FPToInt,
  [InstrStage<3, [RISCV_SLOT1]>],
  [4, 1],
  [Dandelion_WB_Bypass,
   Dandelion_WB_Bypass]>; // fp src

//===----------------------------------------------------------------------===//
// Int→FP  FCVT.S.W / FCVT.S.WU  (pipeline 2 only, result at WB)
//
//  Single integer source available via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_IntToFP : InstrItinData<
  IIC_IntToFP,
  [InstrStage<3, [RISCV_SLOT2]>],
  [4, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // int src: EX2 or EX3

//===----------------------------------------------------------------------===//
// FP Divide / Sqrt  FDIV.S/D, FSQRT.S/D  (pipeline 0 only, result at WB)
//
//  arch.md §FDivFPUPipeline: result forwarded at WB only.
//  FP source operands → WB bypass only.
//===----------------------------------------------------------------------===//

def DandelionIIC_FDiv : InstrItinData<
  IIC_FDiv,
  [InstrStage<28, [RISCV_SLOT0]>],
  [29, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_WB_Bypass,   // fp src1
   Dandelion_WB_Bypass]>; // fp src2

//===----------------------------------------------------------------------===//
// Load  (pipelines 5-6, EX1-EX3, result at WB)
//
//  arch.md §LSU: result forwarded at WB only.
//  Single integer base-address source, available via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_Load : InstrItinData<
  IIC_Load,
  [InstrStage<3, [RISCV_SLOT5, RISCV_SLOT6]>],
  [4, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // base addr: EX2 or EX3

//===----------------------------------------------------------------------===//
// Store  (pipelines 5-6, EX1 only, no destination register)
//
//  rs1 (base address) and rs2 (store data) are integer sources;
//  each is available via EX2 or EX3 bypass.
//  FP store data (fsw/fsd) stalls naturally: FPU DefCycle=4 > UseCycle=1.
//===----------------------------------------------------------------------===//

def DandelionIIC_Store : InstrItinData<
  IIC_Store,
  [InstrStage<1, [RISCV_SLOT5, RISCV_SLOT6]>],
  [1, 1, 1, 1, 1],
  [NoBypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

//===----------------------------------------------------------------------===//
// CSR  (pipeline 7, single cycle, result at EX2)
//
//  Single integer source, available via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_CSR : InstrItinData<
  IIC_CSR,
  [InstrStage<1, [RISCV_SLOT7]>],
  [2, 1, 1],
  [Dandelion_EX2_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // src: EX2 or EX3

//===----------------------------------------------------------------------===//
// Atomic  (pipelines 5-6, EX1-EX3, result at WB)
//
//  Two integer sources (address + data), each via EX2 or EX3 bypass.
//===----------------------------------------------------------------------===//

def DandelionIIC_Atomic : InstrItinData<
  IIC_Atomic,
  [InstrStage<3, [RISCV_SLOT5, RISCV_SLOT6]>],
  [4, 1, 1, 1, 1],
  [Dandelion_WB_Bypass,
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass,   // rs1: EX2 or EX3
   Dandelion_EX2_Bypass, Dandelion_EX3_Bypass]>;  // rs2: EX2 or EX3

//===----------------------------------------------------------------------===//
// NOP / Pseudo  (any ALU pipeline, 1 cycle, no meaningful result)
//===----------------------------------------------------------------------===//

def DandelionIIC_Nop : InstrItinData<
  IIC_Nop,
  [InstrStage<1, [RISCV_SLOT1, RISCV_SLOT2, RISCV_SLOT3,
                  RISCV_SLOT4, RISCV_SLOT5, RISCV_SLOT6, RISCV_SLOT7]>]>;

//===----------------------------------------------------------------------===//
// ProcessorItineraries
//===----------------------------------------------------------------------===//

def RISCVDandelionItineraries : ProcessorItineraries<
  [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2, RISCV_SLOT3,
   RISCV_SLOT4, RISCV_SLOT5, RISCV_SLOT6, RISCV_SLOT7],
  // Three bypass networks (arch.md §Pipeline forwarding table):
  //   EX2_Bypass – ALU / Branch results first available at EX2
  //   EX3_Bypass – same ALU result still valid in EX3 pipeline register
  //   WB_Bypass  – slow-unit results available at WB
  [Dandelion_EX2_Bypass, Dandelion_EX3_Bypass, Dandelion_WB_Bypass],
  [
    DandelionIIC_ALU,
    DandelionIIC_ALU32,
    DandelionIIC_Shift,
    DandelionIIC_Branch,
    DandelionIIC_Jump,
    DandelionIIC_MUL,
    DandelionIIC_DIV,
    DandelionIIC_Load,
    DandelionIIC_Store,
    DandelionIIC_FPU,
    DandelionIIC_FPToInt,
    DandelionIIC_IntToFP,
    DandelionIIC_FDiv,
    DandelionIIC_CSR,
    DandelionIIC_Atomic,
    DandelionIIC_Nop
  ]
> {
  // Setting PacketizerNamespace causes llvm-tblgen to generate a dedicated
  // DFA automaton and a createDandelionDFAPacketizer() method on
  // RISCVGenSubtargetInfo.  Without this, the DFA table is empty and the
  // packetizer accepts an unlimited number of instructions per packet,
  // violating the 2-slot iMul/Div and 2-slot LSU resource constraints.
  let PacketizerNamespace = "Dandelion";
}

//===----------------------------------------------------------------------===//
// SchedMachineModel
//===----------------------------------------------------------------------===//

def RISCVDandelionModel : SchedMachineModel {
  let IssueWidth        = 8;
  let Itineraries       = RISCVDandelionItineraries;
  let LoadLatency       = 4;
  let MispredictPenalty = 5;
  let PostRAScheduler   = 1;
  let CompleteModel     = 0;
}
