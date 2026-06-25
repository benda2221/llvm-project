//===-- RISCVVLIWSlotUtils.h - Dandelion VLIW slot helpers -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVVLIWSLOTUTILS_H
#define LLVM_LIB_TARGET_RISCV_RISCVVLIWSLOTUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>

namespace llvm {

class InstrItineraryData;
class MachineInstr;
class TargetRegisterInfo;

namespace RISCVVLIW {

static constexpr unsigned DandelionSlots = 8;

struct SlotPacket {
  SmallVector<MachineInstr *, DandelionSlots> Instrs;
  SmallVector<int, DandelionSlots> Slots;
};

uint8_t getSlotMask(unsigned SchedClass, const InstrItineraryData *IID);

bool assignSlots(ArrayRef<MachineInstr *> Instrs, const InstrItineraryData *IID,
                 const TargetRegisterInfo *TRI,
                 SmallVectorImpl<int> &AssignedSlots);

void partitionIntoPackets(ArrayRef<MachineInstr *> Instrs,
                          const InstrItineraryData *IID,
                          const TargetRegisterInfo *TRI,
                          SmallVectorImpl<SlotPacket> &Packets);

} // namespace RISCVVLIW
} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVVLIWSLOTUTILS_H
