//===----- RISCVPacketizer.cpp - RISCV packetizer ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RISCV packetizer for the LLVM compiler.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVPacketizer.h"
#include "RISCVInstrInfo.h"
#include "RISCVRegisterInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "packets"

namespace {

    class RISCVPacketizer : public MachineFunctionPass {
    public:
    static char ID;
    RISCVPacketizer() : MachineFunctionPass(ID) {}

    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.setPreservesCFG();
        AU.addRequired<MachineDominatorTreeWrapperPass>();
        AU.addRequired<MachineLoopInfoWrapperPass>();
        AU.addPreserved<MachineDominatorTreeWrapperPass>();
        AU.addPreserved<MachineLoopInfoWrapperPass>();
        MachineFunctionPass::getAnalysisUsage(AU);
    }

    StringRef getPassName() const override { return "RISCV Packetizer"; }

    bool runOnMachineFunction(MachineFunction &Fn) override;

    private:
        const RISCVInstrInfo *TII;
        const RISCVRegisterInfo &TRI;
    };

} // end anonymous namespace

char RISCVPacketizer::ID = 0;

INITIALIZE_PASS_BEGIN(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer", false, false) 
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer", false, false)

RISCVPacketizerList::RISCVPacketizerList(MachineFunction &MF, 
    MachineLoopInfo &MLI, AAResults *AA) 
    : VLIWPacketizerList(MF, MLI, nullptr), 
    RII(MF.getSubtarget<RISCVSubtarget>().getInstrInfo()),
    RRI(*MF.getSubtarget<RISCVSubtarget>().getRegisterInfo()) {
}

bool RISCVPacketizer::runOnMachineFunction(MachineFunction &MF) {
    const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
    const RISCVInstrInfo *RII = ST.getInstrInfo();
    const RISCVRegisterInfo &RRI = *ST.getRegisterInfo();
    
    MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();

    // Instantiate the packetizer.
    RISCVPacketizerList Packetizer(MF, MLI, nullptr);

    // DFA state table should not be empty.
    assert(Packetizer.getResourceTracker() && "Empty DFA table!");

    // Loop over all basic blocks and remove KILL pseudo-instructions
    // These instructions confuse the dependence analysis. Consider:
    // D0 = ...   (Insn 0)
    // R0 = KILL R0, D0 (Insn 1)
    // R0 = ... (Insn 2)
    // Here, Insn 1 will result in the dependence graph not emitting an output
    // dependence between Insn 0 and Insn 2. This can lead to incorrect
    // packetization
    for (MachineBasicBlock &MB : MF) {
        for (MachineInstr &MI : llvm::make_early_inc_range(MB))
        if (MI.isKill())
            MB.erase(&MI);
    }

    // Loop over all of the basic blocks.
    for (MachineBasicBlock &MB : MF) {
        auto Begin = MB.begin(), End = MB.end();
        while (Begin != End) {
            // Find the first non-boundary starting from the end of the last
            // scheduling region.
            MachineBasicBlock::iterator RB = Begin;
            while (RB != End && RII->isSchedulingBoundary(*RB, &MB, MF))
                ++RB;
            // Find the first boundary starting from the beginning of the new
            // region.
            MachineBasicBlock::iterator RE = RB;
            while (RE != End && !RII->isSchedulingBoundary(*RE, &MB, MF))
                ++RE;
            // Add the scheduling boundary if it's not block end.
            if (RE != End)
                ++RE;
            // If RB == End, then RE == End.    
            if (RB != End)
                Packetizer.PacketizeMIs(&MB, RB, RE);
            
                Begin = RE;
        }
    }

    return false;

}

// Initialize packetizer flags.
void RISCVPacketizerList::initPacketizerState() override{
    Dependence = false;
}

bool RISCVPacketizerList::ignorePseudoInstruction(const MachineInstr &MI,
    const MachineBasicBlock *MBB) override {
    if (MI.isDebugInstr())
        return true;

    return false;
}

bool RISCVPacketizerList::isSoloInstruction(const MachineInstr &MI) override {
    // TODO：instructions that can not be packetized with any other instruction.
    return false;
}

bool RISCVPacketizerList::isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) override {
    return false;
}

bool RISCVPacketizerList::isLegalToPruneDependencies(SUnit *SUI, SUnit *SUJ) override {
    // TODO: check if the dependence is legal to prune.
    // we define "shallow dependence" 
    // A maximum of only two instructions in an instruction packet may have a RAW dependency between them
    return false;
}

bool RISCVPacketizerList::shouldAddToPacket(const MachineInstr &MI) override {
    return false;
}

MachineBasicBlock::iterator RISCVPacketizerList::addToPacket(MachineInstr &MI) override {
    return MI.getIterator();
}

void RISCVPacketizerList::endPacket(MachineBasicBlock *MBB,MachineBasicBlock:: iterator MI) override {
    return;
}

//===----------------------------------------------------------------------===//
//                         Public Constructor Functions
//===----------------------------------------------------------------------===//

FunctionPass *llvm::createRISCVPacketizerPass(MachineFunction &MF) {
    return new RISCVPacketizer(MF);
}
