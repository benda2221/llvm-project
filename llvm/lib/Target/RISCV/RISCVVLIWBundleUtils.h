//===-- RISCVVLIWBundleUtils.h - Dandelion bundle helpers ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H
#define LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include <cstdint>

namespace llvm {
class InstrItineraryData;
namespace RISCVVLIW {

uint32_t encodeDandelionSlotMap(ArrayRef<int> AssignedSlots);

bool decodeDandelionSlotMap(const MachineInstr &Bundle, unsigned NumMembers,
                            SmallVectorImpl<int> &AssignedSlots);

bool verifyDandelionSlotMap(const MachineInstr &Bundle,
                            ArrayRef<MachineInstr *> Members,
                            const InstrItineraryData *IID);

/// Finalize a Dandelion instruction packet.
///
/// Every register use in the packet reads the value at packet entry. Register
/// definitions become visible only at packet exit, so member order must not
/// turn a use into an internal bundle read.
MachineInstr &finalizeRISCVBundle(MachineBasicBlock &MBB,
                                  MachineBasicBlock::instr_iterator FirstMI,
                                  MachineBasicBlock::instr_iterator LastMI,
                                  ArrayRef<int> AssignedSlots);

} // namespace RISCVVLIW
} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H
