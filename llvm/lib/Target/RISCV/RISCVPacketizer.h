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

class RISCVInstrInfo;
class RISCVRegisterInfo;
class MachineFunction;
class MachineInstr;
class MachineLoopInfo;
class TargetRegisterClass;

class RISCVPacketizerList : public VLIWPacketizerList {

    // Check if there is a dependence between some instruction already in this
    // packet and this instruction.
    bool Dependence;

private:
    const RISCVInstrInfo *RII;
    const RISCVRegisterInfo &RRI;

public:
    RISCVPacketizerList(MachineFunction &MF, MachineLoopInfo &MLI, AAResults *AA);

    // initPacketizerState - initialize some internal flags.
    void initPacketizerState() override;

    // ignorePseudoInstruction - Ignore bundling of pseudo instructions.
    bool ignorePseudoInstruction(const MachineInstr &MI,
                                 const MachineBasicBlock *MBB) override;

    // isSoloInstruction - return true if instruction MI can not be packetized
    // with any other instruction, which means that MI itself is a packet.
    bool isSoloInstruction(const MachineInstr &MI) override;
    
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
    MachineBasicBlock::iterator addToPacket(MachineInstr &MI) override;
    
    void endPacket(MachineBasicBlock *MBB,
                 MachineBasicBlock::iterator MI) override;

}

} // end namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVPACKETIZER_H