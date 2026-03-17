//===----- RISCVScheduleVLIW.td - RISCV VLIW Scheduling ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines itinerary classes and processor itineraries for a simple
// RISCV VLIW processor model. The DFA packetizer uses these itineraries to
// determine which instructions can be co-issued in a single VLIW packet.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Functional units (issue slots)
//===----------------------------------------------------------------------===//

def RISCV_SLOT0 : FuncUnit;  // ALU / Branch / Load / Store
def RISCV_SLOT1 : FuncUnit;  // ALU / Multiply
def RISCV_SLOT2 : FuncUnit;  // ALU / FPU

//===----------------------------------------------------------------------===//
// Instruction itinerary classes
//===----------------------------------------------------------------------===//

def IIC_ALU     : InstrItinClass;
def IIC_ALU32   : InstrItinClass;
def IIC_Shift   : InstrItinClass;
def IIC_Load    : InstrItinClass;
def IIC_Store   : InstrItinClass;
def IIC_Branch  : InstrItinClass;
def IIC_Jump    : InstrItinClass;
def IIC_MUL     : InstrItinClass;
def IIC_DIV     : InstrItinClass;
def IIC_FPU     : InstrItinClass;
def IIC_CSR     : InstrItinClass;
def IIC_Atomic  : InstrItinClass;
def IIC_Nop     : InstrItinClass;

//===----------------------------------------------------------------------===//
// Processor itineraries
//
// Each InstrItinData maps an itinerary class to the functional units it can
// use, the number of cycles per stage, and operand read/write cycles.
//===----------------------------------------------------------------------===//

def RISCVVLIWItineraries : ProcessorItineraries<
  [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2],  // FuncUnit list
  [],                                         // Bypass list (none)
  [
    // ALU: any slot, 1 cycle
    InstrItinData<IIC_ALU,     [InstrStage<1, [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2]>],
                               [2, 1, 1]>,
    InstrItinData<IIC_ALU32,   [InstrStage<1, [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2]>],
                               [2, 1, 1]>,
    InstrItinData<IIC_Shift,   [InstrStage<1, [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2]>],
                               [2, 1, 1]>,

    // Load: SLOT0 only, 2 cycle load latency
    InstrItinData<IIC_Load,    [InstrStage<1, [RISCV_SLOT0]>],
                               [3, 1]>,

    // Store: SLOT0 only, 1 cycle
    InstrItinData<IIC_Store,   [InstrStage<1, [RISCV_SLOT0]>],
                               [1, 1, 1]>,

    // Branch: SLOT0 only, 1 cycle
    InstrItinData<IIC_Branch,  [InstrStage<1, [RISCV_SLOT0]>],
                               [1, 1, 1]>,

    // Jump: SLOT0 only, 1 cycle
    InstrItinData<IIC_Jump,    [InstrStage<1, [RISCV_SLOT0]>],
                               [2, 1]>,

    // Multiply: SLOT1, 3 cycle latency
    InstrItinData<IIC_MUL,     [InstrStage<3, [RISCV_SLOT1]>],
                               [4, 1, 1]>,

    // Divide: SLOT1, long latency (20 cycles)
    InstrItinData<IIC_DIV,     [InstrStage<20, [RISCV_SLOT1]>],
                               [21, 1, 1]>,

    // FPU: SLOT2, 4 cycle latency
    InstrItinData<IIC_FPU,     [InstrStage<4, [RISCV_SLOT2]>],
                               [5, 1, 1]>,

    // CSR: SLOT0 only, 1 cycle
    InstrItinData<IIC_CSR,     [InstrStage<1, [RISCV_SLOT0]>],
                               [2, 1]>,

    // Atomic: SLOT0 only, 3 cycle latency
    InstrItinData<IIC_Atomic,  [InstrStage<3, [RISCV_SLOT0]>],
                               [4, 1, 1]>,

    // Nop: any slot
    InstrItinData<IIC_Nop,     [InstrStage<1, [RISCV_SLOT0, RISCV_SLOT1, RISCV_SLOT2]>]>
  ]
>;

//===----------------------------------------------------------------------===//
// VLIW scheduling machine model
//===----------------------------------------------------------------------===//

def RISCVVLIWModel : SchedMachineModel {
  let IssueWidth = 3;
  let Itineraries = RISCVVLIWItineraries;
  let LoadLatency = 3;
  let MispredictPenalty = 5;
  let PostRAScheduler = 1;
  let CompleteModel = 0;
}
