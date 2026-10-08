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
STATISTIC(NumRejectedSecondDataPair,
          "[riscv-packetizer] Candidates rejected by a second Data pair");
STATISTIC(NumSlotConstraintSplits, "[riscv-packetizer] Packets split because "
                                   "slot constraints had no solution");

namespace {

/// Return true when \p MO is a source that the emitted instruction really
/// reads in its integer datapath.  Most such operands are explicit.  The one
/// fixed-register exception currently materialized by the Dandelion
/// packetizer is PseudoRET's x1 target.  ABI call arguments, return values and
/// stack-pointer liveness operands remain ordinary implicit uses and must not
/// be mistaken for jump targets.
static bool isShallowHardwareUseOperand(const MachineInstr &MI,
                                        const MachineOperand &MO,
                                        Register Reg) {
  if (!MO.isReg() || !MO.isUse() || MO.isUndef() || MO.getReg() != Reg)
    return false;
  if (!MO.isImplicit())
    return true;
  return MI.getOpcode() == RISCV::PseudoRET && Reg == RISCV::X1;
}

static bool hasShallowHardwareUse(const MachineInstr &MI, Register Reg) {
  for (const MachineOperand &MO : MI.operands())
    if (isShallowHardwareUseOperand(MI, MO, Reg))
      return true;
  return false;
}

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
        !hasShallowHardwareUse(MI, RISCV::X1))
      MI.addOperand(MF, MachineOperand::CreateReg(
                            RISCV::X1, /*isDef=*/false, /*isImp=*/true));
    if (MI.definesRegister(RISCV::X1, TRI))
      HasPriorX1Def = true;
  }
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
  State.clear();
  if (Units.empty() || Units.size() > RISCVVLIW::DandelionSlots)
    return false;

  State.SUnits.append(Units.begin(), Units.end());
  DenseMap<SUnit *, unsigned> Indices;
  for (unsigned I = 0; I != Units.size(); ++I) {
    Indices[Units[I]] = I;
    if (Units.size() > 1 && isSoloInstruction(*Units[I]->getInstr()))
      return false;
  }

  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  for (unsigned I = 0; I != Units.size(); ++I) {
    SUnit *Producer = Units[I];
    for (const SDep &Dep : Producer->Succs) {
      auto It = Indices.find(Dep.getSUnit());
      if (It == Indices.end())
        continue;
      unsigned J = It->second;
      SUnit *Consumer = Units[J];

      switch (Dep.getKind()) {
      case SDep::Data:
        if (DisableShallowDependency || !Dep.isAssignedRegDep() ||
            !hasShallowHardwareUse(*Consumer->getInstr(), Dep.getReg()) ||
            !RISCVVLIW::isShallowDepOK(
                *Producer->getInstr(), RISCVVLIW::ShallowDepRole::Producer,
                IID) ||
            !RISCVVLIW::isShallowDepOK(
                *Consumer->getInstr(), RISCVVLIW::ShallowDepRole::Consumer,
                IID))
          return false;
        if (State.ShallowProducer &&
            (State.ShallowProducer != Producer ||
             State.ShallowConsumer != Consumer))
          return false;
        State.ShallowProducer = Producer;
        State.ShallowConsumer = Consumer;
        State.ShallowDataRegs.push_back(Dep.getReg());
        State.MustPrecede[I][J] = true;
        break;
      case SDep::Output:
        State.MustPrecede[I][J] = true;
        break;
      case SDep::Anti:
        if (RISCVVLIW::isShallowDepOK(
                *Consumer->getInstr(), RISCVVLIW::ShallowDepRole::Producer,
                IID) &&
            RISCVVLIW::isShallowDepOK(
                *Producer->getInstr(), RISCVVLIW::ShallowDepRole::Consumer,
                IID))
          State.MustPrecede[I][J] = true;
        break;
      case SDep::Order:
        if (!Dep.isWeak())
          return false;
        break;
      }
    }
  }

  ResourceTracker->clearResources();
  SmallVector<MachineInstr *, RISCVVLIW::DandelionSlots> Instrs;
  for (SUnit *SU : Units) {
    MachineInstr *MI = SU->getInstr();
    if (!ResourceTracker->canReserveResources(*MI))
      return false;
    ResourceTracker->reserveResources(*MI);
    Instrs.push_back(MI);
  }
  return RISCVVLIW::assignSlots(Instrs, IID, State.MustPrecede,
                                State.AssignedSlots);
}

void RISCVPacketizerList::packetizeSDARegion(
    MachineBasicBlock *MBB, MachineBasicBlock::iterator BeginItr,
    MachineBasicBlock::iterator EndItr) {
  materializeFixedShallowHardwareUses(MF, BeginItr, EndItr);
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
  MachineInstr *Terminator = std::prev(EndItr)->isTerminator()
                                 ? &*std::prev(EndItr)
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
    if (Terminator && !Scheduled.contains(UnitForMI.lookup(Terminator)))
      Seed = UnitForMI.lookup(Terminator);
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
      MBB->splice(EndItr, MBB, Packet.Units[I]->getInstr()->getIterator());
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
  // The DAG may only move instructions within a region.  Keep debug and
  // non-issuing instructions in place, and never move an instruction across a
  // position directive, inline assembly, or a serialized fence.
  SmallVector<MachineInstr *, 64> Original;
  for (auto It = BeginItr; It != EndItr; ++It)
    Original.push_back(&*It);
  for (unsigned I = 0; I < Original.size();) {
    MachineInstr &MI = *Original[I];
    if (MI.isDebugInstr() || MI.isImplicitDef() ||
        shouldNotEnterBundle(MI)) {
      ++I;
      continue;
    }
    unsigned Begin = I;
    do {
      ++I;
    } while (I < Original.size() && !Original[I - 1]->isTerminator() &&
             !Original[I]->isDebugInstr() &&
             !Original[I]->isImplicitDef() &&
             !shouldNotEnterBundle(*Original[I]) &&
             !isSoloInstruction(*Original[I]) &&
             !isSoloInstruction(*Original[I - 1]));
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
  TrialState = PacketState;
  if (!NewSU || !NewSU->getInstr() ||
      TrialState.SUnits.size() >= RISCVVLIW::DandelionSlots)
    return false;

  const InstrItineraryData *IID = MF.getSubtarget().getInstrItineraryData();
  unsigned NewIdx = TrialState.SUnits.size();
  TrialState.SUnits.push_back(NewSU);

  for (unsigned OldIdx = 0; OldIdx != NewIdx; ++OldIdx) {
    SUnit *OldSU = TrialState.SUnits[OldIdx];
    for (const SDep &Dep : OldSU->Succs) {
      if (Dep.getSUnit() != NewSU)
        continue;

      switch (Dep.getKind()) {
      case SDep::Data:
        if (DisableShallowDependency || !Dep.isAssignedRegDep() ||
            !hasShallowHardwareUse(*NewSU->getInstr(), Dep.getReg()) ||
            !RISCVVLIW::isShallowDepOK(
                *OldSU->getInstr(), RISCVVLIW::ShallowDepRole::Producer, IID) ||
            !RISCVVLIW::isShallowDepOK(
                *NewSU->getInstr(), RISCVVLIW::ShallowDepRole::Consumer, IID))
          return false;
        if (TrialState.ShallowProducer &&
            (TrialState.ShallowProducer != OldSU ||
             TrialState.ShallowConsumer != NewSU)) {
          ++NumRejectedSecondDataPair;
          return false;
        }
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
                *NewSU->getInstr(), RISCVVLIW::ShallowDepRole::Producer, IID) &&
            RISCVVLIW::isShallowDepOK(*OldSU->getInstr(),
                                      RISCVVLIW::ShallowDepRole::Consumer, IID))
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
  if (RISCVVLIW::assignSlots(Instrs, IID, TrialState.MustPrecede,
                             TrialState.AssignedSlots))
    return true;
  if (!PacketState.SUnits.empty())
    ++NumSlotConstraintSplits;
  return false;
}

void RISCVPacketizerList::markShallowInternalReads(
    const PacketDependencyState &State) const {
  if (!State.ShallowConsumer)
    return;

  MachineInstr *Consumer = State.ShallowConsumer->getInstr();
  for (Register Reg : State.ShallowDataRegs) {
    bool Marked = false;
    for (MachineOperand &MO : Consumer->operands()) {
      if (!isShallowHardwareUseOperand(*Consumer, MO, Reg))
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
        !hasShallowHardwareUse(*SUI->getInstr(), Dep.getReg()) ||
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
