//===----- RISCVScheduleVLIW.td - RISCV VLIW Scheduling ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines shared VLIW scheduling building blocks for the Dandelion
// processor:
//   - functional units (one per physical pipeline)
//   - instruction itinerary classes (IIC_*)
//
// Dandelion has 8 in-order pipelines.  Each pipeline is modelled as a separate
// FuncUnit so that the itinerary framework can enforce the correct dispatch
// constraints.
//
// Pipeline layout (from arch.md):
//   Slot 0  – FDivFPUPipeline  : FDiv + FPU
//   Slot 1  – ALUFPUPipeline   : ALU  + FPU  (FPToInt  / FCVT.W.S,FCVT.WU.S)
//   Slot 2  – ALUFPUPipeline   : ALU  + FPU  (IntToFP  / FCVT.S.W,FCVT.S.WU)
//   Slot 3  – ALUiMDPipeline   : ALU  + iMul/Div
//   Slot 4  – ALUiMDPipeline   : ALU  + iMul/Div
//   Slot 5  – ALULSUPipeline   : ALU  + LSU
//   Slot 6  – ALULSUPipeline   : ALU  + LSU
//   Slot 7  – ALUBranchPipeline: ALU  + Branch
//
// The concrete processor itineraries and SchedMachineModel are defined in
// RISCVSchedDandelion.td.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Functional units – one per physical pipeline
//===----------------------------------------------------------------------===//

// Pipeline 0: FDiv + FPU  (no ALU)
def RISCV_SLOT0  : FuncUnit;

// Pipeline 1: ALU + FPU  (FPToInt support)
def RISCV_SLOT1  : FuncUnit;

// Pipeline 2: ALU + FPU  (IntToFP support)
def RISCV_SLOT2  : FuncUnit;

// Pipeline 3: ALU + iMul/Div
def RISCV_SLOT3  : FuncUnit;

// Pipeline 4: ALU + iMul/Div
def RISCV_SLOT4  : FuncUnit;

// Pipeline 5: ALU + LSU
def RISCV_SLOT5  : FuncUnit;

// Pipeline 6: ALU + LSU
def RISCV_SLOT6  : FuncUnit;

// Pipeline 7: ALU + Branch
def RISCV_SLOT7  : FuncUnit;

//===----------------------------------------------------------------------===//
// Instruction itinerary classes
//===----------------------------------------------------------------------===//

// Integer ALU (non-shift, non-multiply/divide) – pipelines 1-7
def IIC_ALU     : InstrItinClass;
// RV32-specific ALU word instructions (ADDW/SUBW/…) – pipelines 1-7
def IIC_ALU32   : InstrItinClass;
// Shift instructions – pipelines 1-7
def IIC_Shift   : InstrItinClass;

// Integer multiply – pipelines 3-4
def IIC_MUL     : InstrItinClass;
// Integer divide/remainder – pipelines 3-4 (iterative SRT2)
def IIC_DIV     : InstrItinClass;

// Load  – pipelines 5-6
def IIC_Load    : InstrItinClass;
// Store – pipelines 5-6
def IIC_Store   : InstrItinClass;

// Conditional branch + JAL/JALR – pipeline 7
def IIC_Branch  : InstrItinClass;
def IIC_Jump    : InstrItinClass;

// FPU (add/sub/mul/fma/cmp/fmin/fmax/fsgnj/fcvt scalar) – pipelines 0-2
def IIC_FPU     : InstrItinClass;
// FP-to-Int type conversion (FCVT.W.S / FCVT.WU.S) – pipeline 1 only
def IIC_FPToInt : InstrItinClass;
// Int-to-FP type conversion (FCVT.S.W / FCVT.S.WU) – pipeline 2 only
def IIC_IntToFP : InstrItinClass;
// FP divide / sqrt (FDiv unit, pipeline 0 only)
def IIC_FDiv    : InstrItinClass;

// CSR instructions – treated as single-cycle, pipeline 7 (or SLOT1 fallback)
def IIC_CSR     : InstrItinClass;
// Atomic memory operations – pipelines 5-6 (use LSU)
def IIC_Atomic  : InstrItinClass;
// NOP / pseudo-instructions – any pipeline
def IIC_Nop     : InstrItinClass;
