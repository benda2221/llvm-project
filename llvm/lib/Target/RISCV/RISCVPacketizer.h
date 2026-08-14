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

#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include <vector>

namespace llvm {

class MachineFunction;
class MachineInstr;
class MachineLoopInfo;
class TargetRegisterClass;
class InstrItineraryData;

class RISCVPacketizerList : public VLIWPacketizerList {

    // Check if there is a dependence between some instruction already in this
    // packet and this instruction.
    bool Dependence;

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

    // hasWAWHazardWithSlotConstraint - Returns true if SUI has a WAW
    // dependence on some instruction already in the current packet, and there
    // is no slot available for SUI that is strictly after the latest possible
    // slot of that WAW predecessor.
    bool hasWAWHazardWithSlotConstraint(SUnit *SUI,
                                        const InstrItineraryData *IID);

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
