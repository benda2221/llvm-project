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
#include "llvm/CodeGen/Register.h"
#include <array>
#include <cstdint>

namespace llvm {

class InstrItineraryData;
class MachineInstr;
class MachineOperand;
class SUnit;

namespace RISCVVLIW {

static constexpr unsigned DandelionSlots = 8;

enum class ShallowDepRole { Producer, Consumer };

using SlotOrderMatrix =
    std::array<std::array<bool, DandelionSlots>, DandelionSlots>;

struct SlotPacket {
  SmallVector<MachineInstr *, DandelionSlots> Instrs;
  SmallVector<int, DandelionSlots> Slots;
};

/// Dependency and physical-slot solution for one tentative Dandelion packet.
/// This state is shared by the pre-RA scheduler and the post-RA packetizer so
/// both stages apply the same RAW/WAR/WAW and shallow-dependency rules.
struct PacketLegalityState {
  SmallVector<SUnit *, DandelionSlots> SUnits;
  SUnit *ShallowProducer = nullptr;
  SUnit *ShallowConsumer = nullptr;
  SmallVector<Register, 4> ShallowDataRegs;
  SlotOrderMatrix MustPrecede{};
  SmallVector<int, DandelionSlots> AssignedSlots;

  void clear();
};

uint8_t getSlotMask(unsigned SchedClass, const InstrItineraryData *IID);

/// Return the number of issue slots occupied after a pre-RA pseudo is
/// expanded. Most instructions occupy one slot. PseudoMovAddr and
/// PseudoMovImm each stand for two integer instructions.
unsigned getPreRAIssueSlotCount(const MachineInstr &MI);

bool isShallowDepOK(const MachineInstr &MI, ShallowDepRole Role,
                    const InstrItineraryData *IID);

bool isShallowHardwareUseOperand(const MachineInstr &MI,
                                 const MachineOperand &MO, Register Reg);

bool hasShallowHardwareUse(const MachineInstr &MI, Register Reg);

/// Check all dependencies among \p Units and solve their physical slot
/// assignment. Units must be supplied in execution order. When
/// \p AllowShallowDependencies is false, every true data dependency splits the
/// packet.
bool buildPacketLegalityState(ArrayRef<SUnit *> Units,
                              const InstrItineraryData *IID,
                              bool AllowShallowDependencies,
                              PacketLegalityState &State);

void markShallowInternalReads(const PacketLegalityState &State);

bool assignSlots(ArrayRef<MachineInstr *> Instrs, const InstrItineraryData *IID,
                 const SlotOrderMatrix &MustPrecede,
                 SmallVectorImpl<int> &AssignedSlots);

bool assignSlots(ArrayRef<MachineInstr *> Instrs, const InstrItineraryData *IID,
                 SmallVectorImpl<int> &AssignedSlots);

void partitionIntoPackets(ArrayRef<MachineInstr *> Instrs,
                          const InstrItineraryData *IID,
                          SmallVectorImpl<SlotPacket> &Packets);

} // namespace RISCVVLIW
} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVVLIWSLOTUTILS_H
