//===-- RISCVInsertNOP.cpp - Insert NOP instructions ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that inserts NOP instructions after every 8
// consecutive instructions in the final machine instruction sequence.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define RISCV_INSERT_NOP_NAME "RISC-V Insert NOP pass"

namespace {

class RISCVInsertNOP : public MachineFunctionPass {
public:
  static char ID;
  RISCVInsertNOP() : MachineFunctionPass(ID) {
    initializeRISCVInsertNOPPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return RISCV_INSERT_NOP_NAME; }

private:
  const RISCVInstrInfo *TII;
  bool shouldSkipInstruction(const MachineInstr &MI) const;
};

char RISCVInsertNOP::ID = 0;

bool RISCVInsertNOP::shouldSkipInstruction(const MachineInstr &MI) const {
  // Don't skip terminator instructions - we count them but don't insert NOP after them
  // Skip pseudo instructions (though they should be expanded by this point)
  if (MI.isPseudo())
    return false; // Count pseudo instructions if they still exist

  // Skip already inserted NOPs to avoid double counting
  // Check if this is ADDI x0, x0, 0 (standard NOP)
  if (MI.getOpcode() == RISCV::ADDI && MI.getNumOperands() >= 3 &&
      MI.getOperand(0).isReg() && MI.getOperand(0).getReg() == RISCV::X0 &&
      MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == RISCV::X0 &&
      MI.getOperand(2).isImm() && MI.getOperand(2).getImm() == 0)
    return true;

  // Skip C_NOP if present
  if (MI.getOpcode() == RISCV::C_NOP)
    return true;

  return false;
}

bool RISCVInsertNOP::runOnMachineFunction(MachineFunction &MF) {
  TII = static_cast<const RISCVInstrInfo *>(MF.getSubtarget().getInstrInfo());
  bool Modified = false;
  unsigned InstrCount = 0;

  // Iterate through all basic blocks in the function
  for (auto &MBB : MF) {
    // Iterate through all instructions in the basic block
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      // Skip already inserted NOPs to avoid double counting
      if (shouldSkipInstruction(*MBBI))
        continue;

      // Count this instruction
      InstrCount++;

      // Insert NOP after every 8 instructions
      if (InstrCount % 8 == 0) {
        // Check if the next instruction is a terminator
        auto NextMBBI = std::next(MBBI);
        // Don't insert NOP after terminator instructions
        if (NextMBBI == E || !NextMBBI->isTerminator()) {
          // Get debug location from current instruction or basic block
          DebugLoc DL = MBBI->getDebugLoc();
          if (!DL)
            DL = MBB.findDebugLoc(MBBI);

          // Insert NOP: ADDI x0, x0, 0
          BuildMI(MBB, NextMBBI, DL, TII->get(RISCV::ADDI))
              .addReg(RISCV::X0)
              .addReg(RISCV::X0)
              .addImm(0);

          Modified = true;
        }
      }
    }
  }

  return Modified;
}

} // end of anonymous namespace

INITIALIZE_PASS(RISCVInsertNOP, "riscv-insert-nop", RISCV_INSERT_NOP_NAME,
                false, false)

namespace llvm {

FunctionPass *createRISCVInsertNOPPass() { return new RISCVInsertNOP(); }

} // end of namespace llvm

