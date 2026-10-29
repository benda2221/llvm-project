//===----- RISCVPacketizer.cpp - RISCV packetizer ------------------------===//
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

#include "RISCVPacketizer.h"
#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "RISCVVLIWBundleUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <tuple>

using namespace llvm;

#define DEBUG_TYPE "packets"

static cl::opt<bool> DisableShallowDependency(
    "riscv-disable-shallow-dependency", cl::Hidden,
    cl::desc("Disable Dandelion intra-packet shallow RAW dependencies"),
    cl::init(false));

static cl::opt<bool> EnableSDAPacketizer(
    "riscv-enable-sda-packetizer", cl::Hidden,
    cl::desc("Schedule Dandelion VLIW packets from the remaining critical "
             "path's sink"),
    cl::init(false));

STATISTIC(NumShallowDataEdges,
          "[riscv-packetizer] Accepted shallow Data edges");
STATISTIC(NumShallowEndpointPairs,
          "[riscv-packetizer] Accepted shallow dependency endpoint pairs");
STATISTIC(NumPacketConstraintSplits,
          "[riscv-packetizer] Packets split by dependency or slot constraints");

namespace {

/// PseudoRET expands one-to-one to JALR x0, x1, 0, but its current descriptor
/// does not expose x1 as an operand.  Materialize that fixed hardware use only
/// when an earlier instruction in this packetization region defines x1.  This
/// lets ScheduleDAGInstrs create the real Data edge without globally treating
/// ABI-only implicit operands as shallow sources.
static void materializeFixedShallowHardwareUses(
    MachineFunction &MF, MachineBasicBlock::iterator Begin,
    MachineBasicBlock::iterator End) {
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  bool HasPriorX1Def = false;
  for (auto I = Begin; I != End; ++I) {
    MachineInstr &MI = *I;
    if (MI.getOpcode() == RISCV::PseudoRET && HasPriorX1Def &&
        !RISCVVLIW::hasShallowHardwareUse(MI, RISCV::X1))
      MI.addOperand(MF, MachineOperand::CreateReg(
                            RISCV::X1, /*isDef=*/false, /*isImp=*/true));
    if (MI.definesRegister(RISCV::X1, TRI))
      HasPriorX1Def = true;
  }
}

// The linear packetizer closes a packet after an instruction that occupies
// slot 7. Keep the same boundary in SDA so instructions cannot move across a
// call or another slot-7 operation when the ScheduleDAG lacks a clobber edge.
static bool endsLinearPacket(const MachineInstr &MI,
                             const InstrItineraryData *IID) {
  return MI.isTerminator() || MI.isCall() ||
         RISCVVLIW::getSlotMask(MI.getDesc().getSchedClass(), IID) == 0x80;
}

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

INITIALIZE_PASS_BEGIN(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RISCVPacketizer, "riscv-packetizer", "RISCV Packetizer",
                    false, false)

RISCVPacketizerList::RISCVPacketizerList(MachineFunction &MF,
                                         MachineLoopInfo &MLI, AAResults *AA)
    : VLIWPacketizerList(MF, MLI, nullptr) {}

bool RISCVPacketizer::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "RISCV Packetizer: runOnMachineFunction " << MF.getName()
                    << "\n");

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
      if (EnableSDAPacketizer)
        Packetizer.PacketizeMIsSDA(&MB, RB, RE);
      else
        Packetizer.PacketizeMIs(&MB, RB, RE);
      Changed = true;
    }
  }

  return Changed;
}

// The original packetizer examines a candidate only after the existing packet
// members in program order.  SDA deliberately visits a consumer before its
// producer, so derive all pair constraints from the DAG rather than from the
// order in which candidates were selected.
bool RISCVPacketizerList::buildSDAPacketState(
    ArrayRef<SUnit *> Units, PacketDependencyState &State) {
  if (Units.empty() || Units.size() > RISCVVLIW::DandelionSlots)
    return false;

  for (SUnit *SU : Units)
    if (Units.size() > 1 && isSoloInstruction(*SU->getInstr()))
      return false;

  ResourceTracker->clearResources();
  for (SUnit *SU : Units) {
    MachineInstr *MI = SU->getInstr();
    if (!ResourceTracker->canReserveResources(*MI))
      return false;
    ResourceTracker->reserveResources(*MI);
  }

  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  return RISCVVLIW::buildPacketLegalityState(
      Units, IID, !DisableShallowDependency, State);
}

void RISCVPacketizerList::packetizeSDARegion(
    MachineBasicBlock *MBB, MachineBasicBlock::iterator BeginItr,
    MachineBasicBlock::iterator EndItr) {
  materializeFixedShallowHardwareUses(MF, BeginItr, EndItr);

  // Debug instructions do not issue and must not divide the ScheduleDAG.
  // Keep each one with the preceding issuing instruction when that
  // instruction is moved into its final packet. Leading debug instructions
  // stay before the region.
  DenseMap<MachineInstr *, SmallVector<MachineInstr *, 2>> DebugAfter;
  MachineInstr *PreviousIssue = nullptr;
  for (auto It = BeginItr; It != EndItr; ++It) {
    if (It->isDebugInstr()) {
      if (PreviousIssue)
        DebugAfter[PreviousIssue].push_back(&*It);
    } else {
      PreviousIssue = &*It;
    }
  }

  VLIWScheduler->startBlock(MBB);
  VLIWScheduler->enterRegion(MBB, BeginItr, EndItr,
                             std::distance(BeginItr, EndItr));
  VLIWScheduler->schedule();

  SmallVector<SUnit *, 32> RegionUnits;
  DenseMap<MachineInstr *, SUnit *> UnitForMI;
  for (SUnit &SU : VLIWScheduler->SUnits) {
    RegionUnits.push_back(&SU);
    UnitForMI[SU.getInstr()] = &SU;
  }
  auto LastIssue = EndItr;
  while (LastIssue != BeginItr && std::prev(LastIssue)->isDebugInstr())
    --LastIssue;
  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  MachineInstr *PacketEnd =
      LastIssue != BeginItr && endsLinearPacket(*std::prev(LastIssue), IID)
          ? &*std::prev(LastIssue)
          : nullptr;
  SmallPtrSet<SUnit *, 32> Scheduled;
  SmallPtrSet<SUnit *, 8> InPacket;

  struct PlannedPacket {
    SmallVector<SUnit *, RISCVVLIW::DandelionSlots> Units;
    PacketDependencyState State;
    SmallVector<SUnit *, RISCVVLIW::DandelionSlots> EmissionUnits;
    SmallVector<int, RISCVVLIW::DandelionSlots> EmissionSlots;
  };
  SmallVector<PlannedPacket, 16> PacketStack;

  while (Scheduled.size() < RegionUnits.size()) {
    // Recompute the longest path in the remaining graph after every packet.
    // DAG edges point from earlier instructions to later ones; a sink with the
    // greatest depth is the last instruction on a remaining critical path.
    DenseMap<SUnit *, unsigned> Depth;
    std::function<unsigned(SUnit *)> GetDepth = [&](SUnit *SU) -> unsigned {
      auto It = Depth.find(SU);
      if (It != Depth.end())
        return It->second;
      unsigned Value = std::max(1u, static_cast<unsigned>(SU->Latency));
      for (const SDep &Pred : SU->Preds) {
        SUnit *PSU = Pred.getSUnit();
        if (PSU->isBoundaryNode() || Scheduled.contains(PSU))
          continue;
        Value = std::max(Value, GetDepth(PSU) +
                                    std::max(1u, Pred.getLatency()));
      }
      Depth[SU] = Value;
      return Value;
    };

    auto IsReady = [&](SUnit *SU) {
      for (const SDep &Succ : SU->Succs) {
        SUnit *Next = Succ.getSUnit();
        if (!Next->isBoundaryNode() && !Scheduled.contains(Next) &&
            !InPacket.contains(Next))
          return false;
      }
      return true;
    };

    PlannedPacket Packet;
    SUnit *Seed = nullptr;
    if (PacketEnd && !Scheduled.contains(UnitForMI.lookup(PacketEnd)))
      Seed = UnitForMI.lookup(PacketEnd);
    if (!Seed) {
      for (SUnit *SU : RegionUnits) {
        if (Scheduled.contains(SU) || !IsReady(SU))
          continue;
        if (!Seed || std::make_tuple(GetDepth(SU), SU->NodeNum) >
                         std::make_tuple(GetDepth(Seed), Seed->NodeNum))
          Seed = SU;
      }
    }
    if (!Seed)
      report_fatal_error("Dandelion SDA scheduler has no remaining DAG sink");
    Packet.Units.push_back(Seed);
    if (!buildSDAPacketState(Packet.Units, Packet.State))
      report_fatal_error("Dandelion SDA seed has no legal packet slot");
    InPacket.insert(Seed);

    while (Packet.Units.size() < RISCVVLIW::DandelionSlots) {
      SUnit *Best = nullptr;
      PacketDependencyState BestState;
      for (SUnit *SU : RegionUnits) {
        if (Scheduled.contains(SU) || InPacket.contains(SU) || !IsReady(SU))
          continue;
        SmallVector<SUnit *, RISCVVLIW::DandelionSlots> Trial(Packet.Units);
        Trial.push_back(SU);
        PacketDependencyState TrialState;
        if (!buildSDAPacketState(Trial, TrialState))
          continue;
        if (!Best || std::make_tuple(GetDepth(SU), SU->Preds.size(),
                                     SU->NodeNum) >
                         std::make_tuple(GetDepth(Best), Best->Preds.size(),
                                         Best->NodeNum)) {
          Best = SU;
          BestState = std::move(TrialState);
        }
      }
      if (!Best)
        break;
      Packet.Units.push_back(Best);
      Packet.State = std::move(BestState);
      InPacket.insert(Best);
    }

    for (SUnit *SU : Packet.Units)
      Scheduled.insert(SU);
    InPacket.clear();
    PacketStack.push_back(std::move(Packet));
  }

  // Build the final machine order before creating any BUNDLE: each packet is
  // contiguous, and its members are in physical-slot order so WAW and the
  // bundle's register summary use the same final definition.
  for (PlannedPacket &Packet : llvm::reverse(PacketStack)) {
    SmallVector<unsigned, RISCVVLIW::DandelionSlots> Order;
    for (unsigned I = 0; I != Packet.Units.size(); ++I)
      Order.push_back(I);
    llvm::sort(Order, [&](unsigned A, unsigned B) {
      return Packet.State.AssignedSlots[A] < Packet.State.AssignedSlots[B];
    });
    for (unsigned I : Order) {
      Packet.EmissionUnits.push_back(Packet.Units[I]);
      Packet.EmissionSlots.push_back(Packet.State.AssignedSlots[I]);
      MachineInstr *MI = Packet.Units[I]->getInstr();
      MBB->splice(EndItr, MBB, MI->getIterator());
      auto DebugIt = DebugAfter.find(MI);
      if (DebugIt != DebugAfter.end())
        for (MachineInstr *DMI : DebugIt->second)
          MBB->splice(EndItr, MBB, DMI->getIterator());
    }
  }

  for (PlannedPacket &Packet : llvm::reverse(PacketStack)) {
    markShallowInternalReads(Packet.State);
    NumShallowDataEdges += Packet.State.ShallowDataRegs.size();
    if (Packet.State.ShallowProducer)
      ++NumShallowEndpointPairs;
    MachineInstr *First = Packet.EmissionUnits.front()->getInstr();
    MachineInstr *Last = Packet.EmissionUnits.back()->getInstr();
    RISCVVLIW::finalizeRISCVBundle(
        *MBB, First->getIterator(), std::next(Last->getIterator()),
        Packet.EmissionSlots);
  }

  VLIWScheduler->exitRegion();
  VLIWScheduler->finishBlock();
}

void RISCVPacketizerList::PacketizeMIsSDA(
    MachineBasicBlock *MBB, MachineBasicBlock::iterator BeginItr,
    MachineBasicBlock::iterator EndItr) {
  // The DAG may only move instructions within a region. Debug instructions
  // are transparent, as in the linear packetizer. Non-issuing implicit defs,
  // position directives, inline assembly, and serialized fences remain
  // packet boundaries.
  SmallVector<MachineInstr *, 64> Original;
  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  for (auto It = BeginItr; It != EndItr; ++It)
    Original.push_back(&*It);
  for (unsigned I = 0; I < Original.size();) {
    MachineInstr &MI = *Original[I];
    if (MI.isImplicitDef() || shouldNotEnterBundle(MI)) {
      ++I;
      continue;
    }
    if (MI.isDebugInstr()) {
      ++I;
      continue;
    }
    unsigned Begin = I;
    unsigned LastIssue = I;
    ++I;
    while (I < Original.size()) {
      if (shouldNotEnterBundle(*Original[I]))
        break;
      if (Original[I]->isDebugInstr()) {
        ++I;
        continue;
      }
      if (endsLinearPacket(*Original[LastIssue], IID) ||
          Original[I]->isImplicitDef() || isSoloInstruction(*Original[I]) ||
          isSoloInstruction(*Original[LastIssue]))
        break;
      LastIssue = I;
      ++I;
    }
    auto End = I == Original.size() ? EndItr : Original[I]->getIterator();
    packetizeSDARegion(MBB, Original[Begin]->getIterator(), End);
  }
}

// Initialize packetizer flags.
void RISCVPacketizerList::initPacketizerState() {
  // PacketState deliberately spans all candidates in the current packet and
  // is cleared only when endPacket() emits or abandons that packet.
}

bool RISCVPacketizerList::ignorePseudoInstruction(
    const MachineInstr &MI, const MachineBasicBlock *MBB) {
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
  materializeFixedShallowHardwareUses(MF, BeginItr, EndItr);
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
                 << "Instruction cannot enter the current packet\n  " << MI);
      endPacket(MBB, MI);

      if (!ResourceTracker->canReserveResources(MI) ||
          !commitCandidateToPacketState(SUI))
        report_fatal_error("Dandelion instruction cannot start a legal packet");
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
  if (!NewSU || !NewSU->getInstr() ||
      PacketState.SUnits.size() >= RISCVVLIW::DandelionSlots)
    return false;

  SmallVector<SUnit *, RISCVVLIW::DandelionSlots> TrialUnits(
      PacketState.SUnits);
  TrialUnits.push_back(NewSU);
  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  if (RISCVVLIW::buildPacketLegalityState(
          TrialUnits, IID, !DisableShallowDependency, TrialState))
    return true;
  if (!PacketState.SUnits.empty())
    ++NumPacketConstraintSplits;
  return false;
}

void RISCVPacketizerList::markShallowInternalReads(
    const PacketDependencyState &State) const {
  RISCVVLIW::markShallowInternalReads(State);
}

bool RISCVPacketizerList::commitCandidateToPacketState(SUnit *NewSU) {
  PacketDependencyState TrialState;
  if (!buildTrialPacketState(NewSU, TrialState))
    return false;
  NumShallowDataEdges +=
      TrialState.ShallowDataRegs.size() - PacketState.ShallowDataRegs.size();
  if (!PacketState.ShallowProducer && TrialState.ShallowProducer)
    ++NumShallowEndpointPairs;
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
  if (DisableShallowDependency)
    return false;
  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  bool HasData = false;
  for (const SDep &Dep : SUJ->Succs) {
    if (Dep.getSUnit() != SUI || Dep.getKind() != SDep::Data)
      continue;
    HasData = true;
    if (!Dep.isAssignedRegDep() ||
        !RISCVVLIW::hasShallowHardwareUse(*SUI->getInstr(), Dep.getReg()) ||
        !RISCVVLIW::isShallowDepOK(*SUJ->getInstr(),
                                   RISCVVLIW::ShallowDepRole::Producer, IID) ||
        !RISCVVLIW::isShallowDepOK(*SUI->getInstr(),
                                   RISCVVLIW::ShallowDepRole::Consumer, IID))
      return false;
    if (PacketState.ShallowProducer && (PacketState.ShallowProducer != SUJ ||
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
