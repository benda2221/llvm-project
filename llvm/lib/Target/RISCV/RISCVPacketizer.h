//===----- RISCVPacketizer.h - RISCV packetizer ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the RISCV packetizer for the LLVM compiler.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVPACKETIZER_H
#define LLVM_LIB_TARGET_RISCV_RISCVPACKETIZER_H

#include "RISCVVLIWSlotUtils.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include <utility>

namespace llvm {

class MachineFunction;
class MachineInstr;
class MachineLoopInfo;
class InstrItineraryData;

class RISCVPacketizerList : public VLIWPacketizerList {
    struct PacketDependencyState {
      SmallVector<SUnit *, RISCVVLIW::DandelionSlots> SUnits;
      SUnit *ShallowProducer = nullptr;
      SUnit *ShallowConsumer = nullptr;
      SmallVector<Register, 4> ShallowDataRegs;
      RISCVVLIW::SlotOrderMatrix MustPrecede{};
      SmallVector<int, RISCVVLIW::DandelionSlots> AssignedSlots;

      void clear() {
        SUnits.clear();
        ShallowProducer = nullptr;
        ShallowConsumer = nullptr;
        ShallowDataRegs.clear();
        MustPrecede = {};
        AssignedSlots.clear();
      }
    } PacketState;

private:
    /// Return true for instructions that should not be wrapped into BUNDLE.
    /// These instructions still terminate the current packet boundary.
    bool shouldNotEnterBundle(const MachineInstr &MI) const;

    /// When \p MI is a solo instruction (see isSoloInstruction), return whether
    /// to emit a BUNDLE wrapper for that single-instruction packet. Currently:
    /// non-bundled instruction kinds are handled by shouldNotEnterBundle().
    bool shouldEmitBundleForSoloInstruction(const MachineInstr &MI);

    /// Call finalizeBundle for CurrentPacketMIs over [first, MI), then clear
    /// packet state. Used when a single-instruction packet must be bundled.
    void emitBundleForCurrentPacket(MachineBasicBlock *MBB,
                                    MachineBasicBlock::iterator MI);

    bool buildTrialPacketState(SUnit *NewSU,
                               PacketDependencyState &TrialState) const;

    void markShallowInternalReads(const PacketDependencyState &State) const;

    bool commitCandidateToPacketState(SUnit *NewSU);

public:
    RISCVPacketizerList(MachineFunction &MF, MachineLoopInfo &MLI, AAResults *AA);

    // initPacketizerState - initialize some internal flags.
    void initPacketizerState() override;

    // ignorePseudoInstruction - Ignore bundling of pseudo instructions.
    bool ignorePseudoInstruction(const MachineInstr &MI,
                                 const MachineBasicBlock *MBB) override;

    // isSoloInstruction - true if MI must form a packet by itself (a "solo
    // packet"). Whether it emits BUNDLE is controlled separately.
    // Dandelion fences use this to occupy a dedicated packet.
    bool isSoloInstruction(const MachineInstr &MI) override;

    /// RISCV-specific packetization loop so solo instructions can either:
    ///  - close previous packet and emit their own single-instruction BUNDLE, or
    ///  - just close previous packet and stay unbundled.
    void PacketizeMIs(MachineBasicBlock *MBB,
                      MachineBasicBlock::iterator BeginItr,
                      MachineBasicBlock::iterator EndItr);
    
    // isLegalToPacketizeTogether - Is it legal to packetize SUI and SUJ
    // together.
    bool isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) override;

    // isLegalToPruneDependencies - Is it legal to prune dependence between SUI
    // and SUJ.
    bool isLegalToPruneDependencies(SUnit *SUI, SUnit *SUJ) override;

    // Check if the packetizer should try to add the given instruction to
    // the current packet. One reasons for which it may not be desirable
    // to include an instruction in the current packet could be that it
    // would cause a stall.
    // If this function returns "false", the current packet will be ended,
    // and the instruction will be added to the next packet.
    bool shouldAddToPacket(const MachineInstr &MI) override;

    // addToPacket - Add MI to the current packet.
    // MachineBasicBlock::iterator addToPacket(MachineInstr &MI) override;

    /// Single-instruction packets: solo + shouldEmitBundleForSoloInstruction
    /// decides BUNDLE; non-solo single instructions always emit BUNDLE.
    void endPacket(MachineBasicBlock *MBB,
                   MachineBasicBlock::iterator MI) override;

}; // end class RISCVPacketizerList

} // end namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVPACKETIZER_H
