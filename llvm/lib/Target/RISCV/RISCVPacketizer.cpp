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
    Dependence = false;
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
    if (ResourceAvail && shouldAddToPacket(MI)) {
      for (auto *MJ : CurrentPacketMIs) {
        SUnit *SUJ = MIToSUnit[MJ];
        assert(SUJ && "Missing SUnit Info!");

        LLVM_DEBUG(dbgs() << "  Checking against MJ " << *MJ);
        if (!isLegalToPacketizeTogether(SUI, SUJ)) {
          LLVM_DEBUG(dbgs() << "  Not legal to add MI, try to prune\n");
          if (!isLegalToPruneDependencies(SUI, SUJ)) {
            LLVM_DEBUG(dbgs()
                       << "  Could not prune dependencies for adding MI\n");
            endPacket(MBB, MI);
            break;
          }
          LLVM_DEBUG(dbgs() << "  Pruned dependence for adding MI\n");
        }
      }
    } else {
      LLVM_DEBUG(if (ResourceAvail) dbgs()
                 << "Resources are available, but instruction should not be "
                    "added to packet\n  "
                 << MI);
      endPacket(MBB, MI);
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
    RISCVVLIW::finalizeRISCVBundle(*MBB, MIFirst.getIterator(),
                                   MI.getInstrIterator());
    CurrentPacketMIs.clear();
    ResourceTracker->clearResources();
    LLVM_DEBUG(dbgs() << "End packet\n");
}

// Dandelion slot assignment for each IIC class.
// Slots are numbered 0-7 as defined in RISCVScheduleVLIW.td.
// The DFA assigns the lowest available slot to each instruction, so the
// "last possible slot" for a class is the highest-numbered slot it can use.
//
// Mapping (from InstrItinData entries in RISCVSchedDandelion.td):
//   SLOT0           : IIC_FDiv, IIC_FPU, IIC_FPUToGPR
//   SLOT1           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_FPU, IIC_FPToInt, IIC_Nop
//   SLOT2           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_FPU, IIC_IntToFP, IIC_Nop
//   SLOT3           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_MUL, IIC_DIV, IIC_Nop
//   SLOT4           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_MUL, IIC_DIV, IIC_Nop
//   SLOT5           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_Load, IIC_Store, IIC_Atomic, IIC_Nop
//   SLOT6           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_Load, IIC_Store, IIC_Atomic, IIC_Nop
//   SLOT7           : IIC_ALU, IIC_ALU32, IIC_Shift, IIC_Branch, IIC_Jump,
//                     IIC_Fence, IIC_CSR, IIC_Nop
//
// Returns the set of slots (as a bitmask, bit N = SLOT N) that an instruction
// of the given itinerary class may occupy.
static uint8_t getSlotsForIIC(unsigned SchedClass,
                               const InstrItineraryData *IID) {
  if (!IID || IID->isEmpty())
    return 0xFF; // unknown: allow all slots

  const InstrStage *IS = IID->beginStage(SchedClass);
  const InstrStage *End = IID->endStage(SchedClass);
  uint8_t Mask = 0;
  for (; IS != End; ++IS)
    Mask |= static_cast<uint8_t>(IS->getUnits());
  return Mask;
}

// Returns true if MI has a WAW (output) dependency hazard that prevents it
// from being placed in the current packet.
//
// The rule: if instruction NEW has a WAW dependence on any instruction OLD
// already in the packet, then NEW must be scheduled *after* OLD.  After
// accounting for the slots already consumed by all instructions in the packet,
// we check whether there is any slot that is both:
//   (a) compatible with NEW's IIC, and
//   (b) strictly higher-numbered than every slot that could be used by OLD.
//
// If no such slot exists, NEW cannot be added to this packet.
bool RISCVPacketizerList::hasWAWHazardWithSlotConstraint(
    SUnit *SUI, const InstrItineraryData *IID) {
  MachineInstr *MINew = SUI->getInstr();
  if (!MINew)
    return false;

  unsigned NewSchedClass = MINew->getDesc().getSchedClass();
  uint8_t NewSlots = getSlotsForIIC(NewSchedClass, IID);
  if (!NewSlots)
    return false; // no slot info, be conservative and allow

  // Collect slots already consumed by instructions in the current packet.
  // We track, for each WAW predecessor in the packet, the *highest* slot it
  // could occupy (worst case for the ordering constraint).
  for (MachineInstr *MIOld : CurrentPacketMIs) {
    if (!MIOld)
      continue;

    // Look up the SUnit for this already-packetized instruction.
    auto It = MIToSUnit.find(MIOld);
    if (It == MIToSUnit.end())
      continue;
    SUnit *SUOld = It->second;

    // Check for WAW (output) dependence: MIOld writes a register that MINew
    // also writes.
    bool HasWAW = false;
    for (const SDep &Dep : SUOld->Succs) {
      if (Dep.getSUnit() == SUI && Dep.getKind() == SDep::Output) {
        HasWAW = true;
        break;
      }
    }
    if (!HasWAW)
      continue;

    // MIOld has a WAW dependence on MINew: MINew must follow MIOld.
    // Determine the highest slot that MIOld might occupy.
    unsigned OldSchedClass = MIOld->getDesc().getSchedClass();
    uint8_t OldSlots = getSlotsForIIC(OldSchedClass, IID);
    if (!OldSlots)
      continue;

    // Highest slot index that MIOld can use.
    int MaxOldSlot = 7;
    while (MaxOldSlot >= 0 && !(OldSlots & (1u << MaxOldSlot)))
      --MaxOldSlot;
    if (MaxOldSlot < 0)
      continue;

    // Check if any slot compatible with MINew has index > MaxOldSlot.
    uint8_t LaterSlots = NewSlots & ~((1u << (MaxOldSlot + 1)) - 1u);
    if (!LaterSlots) {
      LLVM_DEBUG(dbgs() << "WAW slot hazard: "
                        << MINew->getMF()->getName() << ": "
                        << *MINew << "  must follow  " << *MIOld
                        << "  but no later slot available\n");
      return true; // cannot pack: no slot available after OldSlot
    }
  }
  return false;
}

bool RISCVPacketizerList::isLegalToPacketizeTogether(SUnit *SUI, SUnit *SUJ) {
    // Reject RAW (data) dependencies.
    for (unsigned i = 0; i < SUJ->Succs.size(); ++i) {
        if (SUJ->Succs[i].getSUnit() != SUI)
            continue;
        SDep::Kind DepType = SUJ->Succs[i].getKind();
        if (DepType == SDep::Data) {
            return false;
        }
    }

    // Reject WAW dependencies where the later-writing instruction has no
    // available slot after the earlier-writing instruction's slot range.
    const InstrItineraryData *IID =
        MF.getSubtarget().getInstrItineraryData();
    if (hasWAWHazardWithSlotConstraint(SUI, IID))
        return false;

    return true;
}

bool RISCVPacketizerList::isLegalToPruneDependencies(SUnit *SUI, SUnit *SUJ) {
    // TODO: check if the dependence is legal to prune.
    // we define "shallow dependence" 
    // A maximum of only two instructions in an instruction packet may have a RAW dependency between them
    return false;
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
