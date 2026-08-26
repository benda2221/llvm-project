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
#include "RISCVSubtarget.h"
#include "RISCVVLIWBundleUtils.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include <cassert>

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

    };

} // end anonymous namespace

char RISCVPacketizer::ID = 0;

INITIALIZE_PASS_BEGIN(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer", false, false) 
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer", false, false)

RISCVPacketizerList::RISCVPacketizerList(MachineFunction &MF, 
    MachineLoopInfo &MLI, AAResults *AA) 
    : VLIWPacketizerList(MF, MLI, nullptr) {}

bool RISCVPacketizer::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "RISCV Packetizer: runOnMachineFunction " << MF.getName() << "\n");

    const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
    const InstrItineraryData *IID = ST.getInstrItineraryData();
    if (!IID || IID->isEmpty())
        return false;

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

    bool Changed = false;

    // Loop over all of the basic blocks.
    for (MachineBasicBlock &MB : MF) {
        auto End = MB.end();
        // The packetizable region covers all instructions in the block,
        // including blocks that consist solely of terminator instructions
        // (e.g., a standalone unconditional branch).
        MachineBasicBlock::iterator RB = MB.begin();
        MachineBasicBlock::iterator RE = RB;
        // Advance past all non-terminator instructions.
        while (RE != End && !RE->isTerminator())
            ++RE;
        // Include all consecutive terminator instructions so that every
        // branch / jump in the block is wrapped into a BUNDLE and can be
        // padded to the full issue-width by RISCVPackPadding.
        while (RE != End && RE->isTerminator())
            ++RE;
        if (RB != RE) {
            Packetizer.PacketizeMIs(&MB, RB, RE);
            Changed = true;
        }
    }

    return Changed;

}

// Initialize packetizer flags.
void RISCVPacketizerList::initPacketizerState() {
  // PacketState deliberately spans all candidates in the current packet and
  // is cleared only when endPacket() emits or abandons that packet.
}

bool RISCVPacketizerList::ignorePseudoInstruction(const MachineInstr &MI,
    const MachineBasicBlock *MBB) {
    if (MI.isDebugInstr())
        return true;
    if (MI.isImplicitDef())
        return true;

    return false;
}

bool RISCVPacketizerList::shouldNotEnterBundle(const MachineInstr &MI) const {
    // Position directives and inline assembly are packet boundaries but should
    // not be wrapped in BUNDLE. Inline assembly is emitted immediately after
    // the preceding packet; authors are responsible for supplying a complete,
    // legal VLIW packet when the assembly string contains instructions.
    return MI.isPosition() || MI.isInlineAsm();
}

bool RISCVPacketizerList::isSoloInstruction(const MachineInstr &MI) {
    switch (MI.getOpcode()) {
    case RISCV::FENCE:
    case RISCV::FENCE_I:
    case RISCV::FENCE_TSO:
        return true;
    default:
        return false;
    }
}

bool RISCVPacketizerList::shouldEmitBundleForSoloInstruction(
    const MachineInstr &MI) {
    assert(isSoloInstruction(MI) &&
           "shouldEmitBundleForSoloInstruction is only for solo instructions");
    // Current rule: position/symbolic instructions do not get a BUNDLE.
    if (MI.isPosition())
        return false;
    return true;
}

void RISCVPacketizerList::PacketizeMIs(MachineBasicBlock *MBB,
                                       MachineBasicBlock::iterator BeginItr,
                                       MachineBasicBlock::iterator EndItr) {
  assert(VLIWScheduler && "VLIW Scheduler is not initialized!");
  VLIWScheduler->startBlock(MBB);
  VLIWScheduler->enterRegion(MBB, BeginItr, EndItr,
                             std::distance(BeginItr, EndItr));
  VLIWScheduler->schedule();

  LLVM_DEBUG({
    dbgs() << "Scheduling DAG of the packetize region\n";
    VLIWScheduler->dump();
  });

  MIToSUnit.clear();
  for (SUnit &SU : VLIWScheduler->SUnits)
    MIToSUnit[SU.getInstr()] = &SU;

  for (; BeginItr != EndItr; ++BeginItr) {
    MachineInstr &MI = *BeginItr;
    initPacketizerState();

    if (shouldNotEnterBundle(MI)) {
      LLVM_DEBUG(dbgs() << "RISCVPacketizer: instruction not entering bundle: "
                        << MI << '\n');
      // Close packet before this instruction and leave this instruction
      // unbundled.
      endPacket(MBB, MI);
      continue;
    }

    if (isSoloInstruction(MI)) {
      // First close the packet before this solo instruction.
      endPacket(MBB, MI);
      // If this solo instruction should be bundled, emit a one-instruction
      // packet [MI, next(MI)).
      if (shouldEmitBundleForSoloInstruction(MI)) {
        SUnit *SoloSU = MIToSUnit[&MI];
        assert(SoloSU && "Missing SUnit Info!");
        if (!commitCandidateToPacketState(SoloSU))
          report_fatal_error("Dandelion solo instruction has no legal slot");
        addToPacket(MI);
        auto Next = std::next(BeginItr);
        endPacket(MBB, Next);
      }
      continue;
    }

    if (MI.isImplicitDef()) {
      LLVM_DEBUG(dbgs() << "RISCVPacketizer: implicit-def not entering bundle: "
                        << MI << '\n');
      endPacket(MBB, MI);
      continue;
    }

    if (ignorePseudoInstruction(MI, MBB))
      continue;

    SUnit *SUI = MIToSUnit[&MI];
    assert(SUI && "Missing SUnit Info!");

    LLVM_DEBUG(dbgs() << "Checking resources for adding MI to packet " << MI);
    bool ResourceAvail = ResourceTracker->canReserveResources(MI);
    LLVM_DEBUG({
      if (ResourceAvail)
        dbgs() << "  Resources are available for adding MI to packet\n";
      else
        dbgs() << "  Resources NOT available\n";
    });
    bool CandidateOK = ResourceAvail && shouldAddToPacket(MI) &&
                       commitCandidateToPacketState(SUI);
    if (!CandidateOK) {
      LLVM_DEBUG(if (ResourceAvail) dbgs()
                 << "Instruction cannot enter the current packet\n  "
                 << MI);
      endPacket(MBB, MI);

      if (!ResourceTracker->canReserveResources(MI) ||
          !commitCandidateToPacketState(SUI))
        report_fatal_error(
            "Dandelion instruction cannot start a legal packet");
    }

    LLVM_DEBUG(dbgs() << "* Adding MI to packet " << MI << '\n');
    BeginItr = addToPacket(MI);
  }

  endPacket(MBB, EndItr);
  VLIWScheduler->exitRegion();
  VLIWScheduler->finishBlock();
}

void RISCVPacketizerList::emitBundleForCurrentPacket(
    MachineBasicBlock *MBB, MachineBasicBlock::iterator MI) {
    LLVM_DEBUG({
        dbgs() << "Finalizing packet:\n";
        unsigned Idx = 0;
        for (MachineInstr *M : CurrentPacketMIs) {
            unsigned R = ResourceTracker->getUsedResources(Idx++);
            dbgs() << " * [res:0x" << utohexstr(R) << "] " << *M;
        }
    });
    MachineInstr &MIFirst = *CurrentPacketMIs.front();
    assert(PacketState.AssignedSlots.size() == CurrentPacketMIs.size() &&
           "Dandelion packet slot map is out of sync");
    RISCVVLIW::finalizeRISCVBundle(*MBB, MIFirst.getIterator(),
                                   MI.getInstrIterator(),
                                   PacketState.AssignedSlots);
    CurrentPacketMIs.clear();
    PacketState.clear();
    ResourceTracker->clearResources();
    LLVM_DEBUG(dbgs() << "End packet\n");
}

bool RISCVPacketizerList::buildTrialPacketState(
    SUnit *NewSU, PacketDependencyState &TrialState) const {
  TrialState = PacketState;
  if (!NewSU || !NewSU->getInstr() ||
      TrialState.SUnits.size() >= RISCVVLIW::DandelionSlots)
    return false;

  const InstrItineraryData *IID =
      MF.getSubtarget().getInstrItineraryData();
  unsigned NewIdx = TrialState.SUnits.size();
  TrialState.SUnits.push_back(NewSU);

  for (unsigned OldIdx = 0; OldIdx != NewIdx; ++OldIdx) {
    SUnit *OldSU = TrialState.SUnits[OldIdx];
    for (const SDep &Dep : OldSU->Succs) {
      if (Dep.getSUnit() != NewSU)
        continue;

      switch (Dep.getKind()) {
      case SDep::Data:
        if (!Dep.isAssignedRegDep() ||
            !RISCVVLIW::isShallowDepOK(
                *OldSU->getInstr(), RISCVVLIW::ShallowDepRole::Producer,
                IID) ||
            !RISCVVLIW::isShallowDepOK(
                *NewSU->getInstr(), RISCVVLIW::ShallowDepRole::Consumer,
                IID))
          return false;
        if (TrialState.ShallowProducer &&
            (TrialState.ShallowProducer != OldSU ||
             TrialState.ShallowConsumer != NewSU))
          return false;
        TrialState.ShallowProducer = OldSU;
        TrialState.ShallowConsumer = NewSU;
        TrialState.ShallowDataRegs.push_back(Dep.getReg());
        TrialState.MustPrecede[OldIdx][NewIdx] = true;
        break;
      case SDep::Output:
        TrialState.MustPrecede[OldIdx][NewIdx] = true;
        break;
      case SDep::Anti:
        if (RISCVVLIW::isShallowDepOK(
                *NewSU->getInstr(), RISCVVLIW::ShallowDepRole::Producer,
                IID) &&
            RISCVVLIW::isShallowDepOK(
                *OldSU->getInstr(), RISCVVLIW::ShallowDepRole::Consumer,
                IID))
          TrialState.MustPrecede[OldIdx][NewIdx] = true;
        break;
      case SDep::Order:
        if (!Dep.isWeak())
          return false;
        break;
      }
    }
  }

  SmallVector<MachineInstr *, RISCVVLIW::DandelionSlots> Instrs;
  for (SUnit *SU : TrialState.SUnits)
    Instrs.push_back(SU->getInstr());
  return RISCVVLIW::assignSlots(Instrs, IID, TrialState.MustPrecede,
                                TrialState.AssignedSlots);
}

void RISCVPacketizerList::markShallowInternalReads(
    const PacketDependencyState &State) const {
  if (!State.ShallowConsumer)
    return;

  MachineInstr *Consumer = State.ShallowConsumer->getInstr();
  for (Register Reg : State.ShallowDataRegs) {
    bool Marked = false;
    for (MachineOperand &MO : Consumer->operands()) {
      if (!MO.isReg() || !MO.isUse() || MO.isImplicit() ||
          MO.getReg() != Reg)
        continue;
      MO.setIsInternalRead(true);
      Marked = true;
    }
    assert(Marked && "SDep::Data register is not a consumer operand");
  }
}

bool RISCVPacketizerList::commitCandidateToPacketState(SUnit *NewSU) {
  PacketDependencyState TrialState;
  if (!buildTrialPacketState(NewSU, TrialState))
    return false;
  markShallowInternalReads(TrialState);
  PacketState = std::move(TrialState);
  return true;
}

bool RISCVPacketizerList::isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) {
  for (const SDep &Dep : SUJ->Succs) {
    if (Dep.getSUnit() != SUI)
      continue;
    if (Dep.getKind() == SDep::Data ||
        (Dep.getKind() == SDep::Order && !Dep.isWeak()))
      return false;
  }
  return true;
}

bool RISCVPacketizerList::isLegalToPruneDependencies(SUnit *SUI, SUnit *SUJ) {
  const InstrItineraryData *IID =
      MF.getSubtarget().getInstrItineraryData();
  bool HasData = false;
  for (const SDep &Dep : SUJ->Succs) {
    if (Dep.getSUnit() != SUI || Dep.getKind() != SDep::Data)
      continue;
    HasData = true;
    if (!Dep.isAssignedRegDep() ||
        !RISCVVLIW::isShallowDepOK(
            *SUJ->getInstr(), RISCVVLIW::ShallowDepRole::Producer, IID) ||
        !RISCVVLIW::isShallowDepOK(
            *SUI->getInstr(), RISCVVLIW::ShallowDepRole::Consumer, IID))
      return false;
    if (PacketState.ShallowProducer &&
        (PacketState.ShallowProducer != SUJ ||
         PacketState.ShallowConsumer != SUI))
      return false;
  }
  return HasData;
}

bool RISCVPacketizerList::shouldAddToPacket(const MachineInstr &MI) {
    // If SLOT7 (res bit 0x80, IIC_Jump) is already occupied by an instruction
    // in the current packet, no further instruction may join this packet.
    for (unsigned i = 0; i < CurrentPacketMIs.size(); ++i) {
        if (ResourceTracker->getUsedResources(i) & 0x80)
            return false;
    }
    return true;
}

void RISCVPacketizerList::endPacket(MachineBasicBlock *MBB,
                                    MachineBasicBlock::iterator MI) {
  if (CurrentPacketMIs.empty()) {
    PacketState.clear();
    ResourceTracker->clearResources();
    return;
  }

  if (CurrentPacketMIs.size() != 1) {
    emitBundleForCurrentPacket(MBB, MI);
    return;
  }

  MachineInstr *Only = CurrentPacketMIs.front();

  // Single instruction, not a "solo packet" kind: always emit BUNDLE (e.g. NOP
  // padding to fixed slot count).
  if (!isSoloInstruction(*Only)) {
    emitBundleForCurrentPacket(MBB, MI);
    return;
  }

  // Solo packet: BUNDLE or not is decided by shouldEmitBundleForSoloInstruction
  // (e.g. isPosition() => no BUNDLE).
  if (shouldEmitBundleForSoloInstruction(*Only))
    emitBundleForCurrentPacket(MBB, MI);
  else
    VLIWPacketizerList::endPacket(MBB, MI);
}

//===----------------------------------------------------------------------===//
//                         Public Constructor Functions
//===----------------------------------------------------------------------===//

FunctionPass *llvm::createRISCVPacketizerPass() {
  return new RISCVPacketizer();
}
