//===- RISCVRelayout.cpp - RISC-V relayout (BranchRelaxation algorithm) ---===//
//
// Same implementation as llvm/lib/CodeGen/BranchRelaxation.cpp, registered as
// the "RISC-V relayout pass" for pipeline placement.  Pass name is unchanged.
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "RISCVVLIWBundleUtils.h"
#include "RISCVVLIWSlotUtils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "riscv-relayout"

// Return the first branch instruction reachable from MI.
// If MI is a BUNDLE, search its members via instr_iterator; otherwise return
// &MI when it is itself a branch, or nullptr if it is neither.
static MachineInstr *getEffectiveBranchMI(MachineInstr &MI) {
  if (!MI.isBundle())
    return MI.isBranch() ? &MI : nullptr;
  for (auto It = std::next(MI.getIterator()),
            E  = MI.getParent()->instr_end();
       It != E && It->isInsideBundle(); ++It)
    if (It->isBranch())
      return &*It;
  return nullptr;
}

STATISTIC(RISCVRelayoutNumSplit, "[riscv-relayout] Number of basic blocks split");
STATISTIC(RISCVRelayoutNumConditionalRelaxed, "[riscv-relayout] Number of conditional branches relaxed");
STATISTIC(RISCVRelayoutNumUnconditionalRelaxed, "[riscv-relayout] Number of unconditional branches relaxed");

#define RISCV_RELAYOUT_PASS_NAME "RISC-V relayout pass"

// Number of VLIW slots in the Dandelion processor (mirrors RISCVPackPadding).
static constexpr unsigned DandelionSlotsRL = 8;

// Returns true if MI is one of the NOP forms inserted by RISCVPackPadding:
//   SLOT0: FEQ.S x0, f0, f0
//   SLOT1-7: ADDI x0, x0, 0
static bool isNopMI(const MachineInstr &MI) {
  if (MI.getOpcode() == RISCV::ADDI) {
    return MI.getOperand(0).isReg() &&
           MI.getOperand(0).getReg() == RISCV::X0 &&
           MI.getOperand(1).isReg() &&
           MI.getOperand(1).getReg() == RISCV::X0 &&
           MI.getOperand(2).isImm() &&
           MI.getOperand(2).getImm() == 0;
  }
  if (MI.getOpcode() == RISCV::FEQ_S) {
    return MI.getOperand(0).isReg() &&
           MI.getOperand(0).getReg() == RISCV::X0;
  }
  return false;
}

namespace {

class RISCVRelayoutImpl {
  /// BasicBlockInfo - Information about the offset and size of a single
  /// basic block.
  struct BasicBlockInfo {
    /// Offset - Distance from the beginning of the function to the beginning
    /// of this basic block.
    ///
    /// The offset is always aligned as required by the basic block.
    unsigned Offset = 0;

    /// Size - Size of the basic block in bytes.  If the block contains
    /// inline assembly, this is a worst case estimate.
    ///
    /// The size does not include any alignment padding whether from the
    /// beginning of the block, or from an aligned jump table at the end.
    unsigned Size = 0;

    BasicBlockInfo() = default;

    /// Compute the offset immediately following this block. \p MBB is the next
    /// block.
    unsigned postOffset(const MachineBasicBlock &MBB) const {
      const unsigned PO = Offset + Size;
      const Align Alignment = MBB.getAlignment();
      const Align ParentAlign = MBB.getParent()->getAlignment();
      if (Alignment <= ParentAlign)
        return alignTo(PO, Alignment);

      // The alignment of this MBB is larger than the function's alignment, so
      // we can't tell whether or not it will insert nops. Assume that it will.
      return alignTo(PO, Alignment) + Alignment.value() - ParentAlign.value();
    }
  };

  SmallVector<BasicBlockInfo, 16> BlockInfo;

  // The basic block after which trampolines are inserted. This is the last
  // basic block that isn't in the cold section.
  MachineBasicBlock *TrampolineInsertionPoint = nullptr;
  SmallDenseSet<std::pair<MachineBasicBlock *, MachineBasicBlock *>>
      RelaxedUnconditionals;
  std::unique_ptr<RegScavenger> RS;
  LivePhysRegs LiveRegs;

  MachineFunction *MF = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  const TargetMachine *TM = nullptr;
  // RISCV-specific pointers for VLIW bundle construction in new blocks.
  const RISCVInstrInfo *RII = nullptr;
  const InstrItineraryData *IID = nullptr;

  bool relaxBranchInstructions();
  void scanFunction();

  bool bundleAwareAnalyzeBranch(MachineBasicBlock &MBB,
                                 MachineBasicBlock *&TBB,
                                 MachineBasicBlock *&FBB,
                                 SmallVectorImpl<MachineOperand> &Cond);
  unsigned bundleAwareRemoveBranch(MachineBasicBlock &MBB,
                                    unsigned &BBSize);

  MachineBasicBlock *createNewBlockAfter(MachineBasicBlock &OrigMBB);
  MachineBasicBlock *createNewBlockAfter(MachineBasicBlock &OrigMBB,
                                         const BasicBlock *BB);

  MachineBasicBlock *splitBlockBeforeInstr(MachineInstr &MI,
                                           MachineBasicBlock *DestBB);
  void adjustBlockOffsets(MachineBasicBlock &Start);
  void adjustBlockOffsets(MachineBasicBlock &Start,
                          MachineFunction::iterator End);
  bool isBlockInRange(const MachineInstr &MI,
                      const MachineBasicBlock &BB) const;

  bool fixupConditionalBranch(MachineInstr &MI);
  bool fixupUnconditionalBranch(MachineInstr &MI);
  void wrapInVLIWBundle(MachineBasicBlock &NewBB);
  void wrapStandaloneBranchesInMBB(MachineBasicBlock &MBB);
  uint64_t computeBlockSize(const MachineBasicBlock &MBB) const;
  unsigned getInstrOffset(const MachineInstr &MI) const;
  void dumpBBs();
  void verify();

public:
  bool run(MachineFunction &MF);
};

class RISCVRelayout : public MachineFunctionPass {
public:
  static char ID;

  RISCVRelayout() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return RISCVRelayoutImpl().run(MF);
  }

  StringRef getPassName() const override { return RISCV_RELAYOUT_PASS_NAME; }
};

} // end anonymous namespace

char RISCVRelayout::ID = 0;


INITIALIZE_PASS(RISCVRelayout, DEBUG_TYPE, RISCV_RELAYOUT_PASS_NAME, false,
                false)

/// verify - check BBOffsets, BBSizes, alignment of islands
void RISCVRelayoutImpl::verify() {
#ifndef NDEBUG
  unsigned PrevNum = MF->begin()->getNumber();
  for (MachineBasicBlock &MBB : *MF) {
    const unsigned Num = MBB.getNumber();
    assert(!Num || BlockInfo[PrevNum].postOffset(MBB) <= BlockInfo[Num].Offset);
    assert(BlockInfo[Num].Size == computeBlockSize(MBB));
    PrevNum = Num;
  }

  for (MachineBasicBlock &MBB : *MF) {
    // Use getFirstInstrTerminator() so that branches inside BUNDLE containers
    // are visible; skip BUNDLE headers and debug instructions.
    for (auto J = MBB.getFirstInstrTerminator(); J != MBB.instr_end(); ++J) {
      if (J->isBundle() || J->isDebugInstr())
        continue;
      MachineInstr &MI = *J;
      if (!MI.isConditionalBranch() && !MI.isUnconditionalBranch())
        continue;
      if (MI.getOpcode() == TargetOpcode::FAULTING_OP)
        continue;
      MachineBasicBlock *DestBB = TII->getBranchDestBlock(MI);
      assert(isBlockInRange(MI, *DestBB) ||
             RelaxedUnconditionals.contains({&MBB, DestBB}));
    }
  }
#endif
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
/// print block size and offset information - debugging
LLVM_DUMP_METHOD void RISCVRelayoutImpl::dumpBBs() {
  for (auto &MBB : *MF) {
    const BasicBlockInfo &BBI = BlockInfo[MBB.getNumber()];
    dbgs() << format("%%bb.%u\toffset=%08x\t", MBB.getNumber(), BBI.Offset)
           << format("size=%#x\n", BBI.Size);
  }
}
#endif

/// scanFunction - Do the initial scan of the function, building up
/// information about each block.
void RISCVRelayoutImpl::scanFunction() {
  BlockInfo.clear();
  BlockInfo.resize(MF->getNumBlockIDs());

  TrampolineInsertionPoint = nullptr;
  RelaxedUnconditionals.clear();

  // First thing, compute the size of all basic blocks, and see if the function
  // has any inline assembly in it. If so, we have to be conservative about
  // alignment assumptions, as we don't know for sure the size of any
  // instructions in the inline assembly. At the same time, place the
  // trampoline insertion point at the end of the hot portion of the function.
  for (MachineBasicBlock &MBB : *MF) {
    BlockInfo[MBB.getNumber()].Size = computeBlockSize(MBB);

    if (MBB.getSectionID() != MBBSectionID::ColdSectionID)
      TrampolineInsertionPoint = &MBB;
  }

  // Compute block offsets and known bits.
  adjustBlockOffsets(*MF->begin());

  if (TrampolineInsertionPoint == nullptr) {
    LLVM_DEBUG(dbgs() << "  No suitable trampoline insertion point found in "
                      << MF->getName() << ".\n");
  }
}

/// computeBlockSize - Compute the size for MBB.
uint64_t
RISCVRelayoutImpl::computeBlockSize(const MachineBasicBlock &MBB) const {
  uint64_t Size = 0;
  for (const MachineInstr &MI : MBB)
    Size += TII->getInstSizeInBytes(MI);
  return Size;
}

/// getInstrOffset - Return the current offset of the specified machine
/// instruction from the start of the function.  This offset changes as stuff is
/// moved around inside the function.
unsigned RISCVRelayoutImpl::getInstrOffset(const MachineInstr &MI) const {
  const MachineBasicBlock *MBB = MI.getParent();

  // The offset is composed of two things: the sum of the sizes of all MBB's
  // before this instruction's block, and the offset from the start of the block
  // it is in.
  unsigned Offset = BlockInfo[MBB->getNumber()].Offset;

  // If MI is a bundle member, resolve to the enclosing BUNDLE header so the
  // block-level iterator can find it (bundle members are invisible to it).
  // The PC of any instruction inside a VLIW packet equals the packet's start.
  const MachineInstr *Target = &MI;
  if (Target->isInsideBundle()) {
    auto It = Target->getIterator();
    while (It->isInsideBundle())
      --It;
    Target = &*It;
  }
  for (MachineBasicBlock::const_iterator I = MBB->begin(); &*I != Target; ++I) {
    assert(I != MBB->end() && "Didn't find MI in its own basic block?");
    Offset += TII->getInstSizeInBytes(*I);
  }

  return Offset;
}

void RISCVRelayoutImpl::adjustBlockOffsets(MachineBasicBlock &Start) {
  adjustBlockOffsets(Start, MF->end());
}

void RISCVRelayoutImpl::adjustBlockOffsets(MachineBasicBlock &Start,
                                          MachineFunction::iterator End) {
  unsigned PrevNum = Start.getNumber();
  for (auto &MBB :
       make_range(std::next(MachineFunction::iterator(Start)), End)) {
    unsigned Num = MBB.getNumber();
    // Get the offset and known bits at the end of the layout predecessor.
    // Include the alignment of the current block.
    BlockInfo[Num].Offset = BlockInfo[PrevNum].postOffset(MBB);

    PrevNum = Num;
  }
}

/// Insert a new empty MachineBasicBlock and insert it after \p OrigMBB
MachineBasicBlock *
RISCVRelayoutImpl::createNewBlockAfter(MachineBasicBlock &OrigBB) {
  return createNewBlockAfter(OrigBB, OrigBB.getBasicBlock());
}

/// Insert a new empty MachineBasicBlock with \p BB as its BasicBlock
/// and insert it after \p OrigMBB
MachineBasicBlock *
RISCVRelayoutImpl::createNewBlockAfter(MachineBasicBlock &OrigMBB,
                                      const BasicBlock *BB) {
  // Create a new MBB for the code after the OrigBB.
  MachineBasicBlock *NewBB = MF->CreateMachineBasicBlock(BB);
  MF->insert(++OrigMBB.getIterator(), NewBB);

  // Place the new block in the same section as OrigBB
  NewBB->setSectionID(OrigMBB.getSectionID());
  NewBB->setIsEndSection(OrigMBB.isEndSection());
  OrigMBB.setIsEndSection(false);

  // Insert an entry into BlockInfo to align it properly with the block numbers.
  BlockInfo.insert(BlockInfo.begin() + NewBB->getNumber(), BasicBlockInfo());

  return NewBB;
}

/// Split the basic block containing MI into two blocks, which are joined by
/// an unconditional branch.  Update data structures and renumber blocks to
/// account for this change and returns the newly created block.
MachineBasicBlock *
RISCVRelayoutImpl::splitBlockBeforeInstr(MachineInstr &MI,
                                        MachineBasicBlock *DestBB) {
  MachineBasicBlock *OrigBB = MI.getParent();

  // Create a new MBB for the code after the OrigBB.
  MachineBasicBlock *NewBB =
      MF->CreateMachineBasicBlock(OrigBB->getBasicBlock());
  MF->insert(++OrigBB->getIterator(), NewBB);

  // Place the new block in the same section as OrigBB.
  NewBB->setSectionID(OrigBB->getSectionID());
  NewBB->setIsEndSection(OrigBB->isEndSection());
  OrigBB->setIsEndSection(false);

  // Splice the instructions starting with MI over to NewBB.
  NewBB->splice(NewBB->end(), OrigBB, MI.getIterator(), OrigBB->end());

  // Add an unconditional branch from OrigBB to NewBB.
  // Note the new unconditional branch is not being recorded.
  // There doesn't seem to be meaningful DebugInfo available; this doesn't
  // correspond to anything in the source.
  TII->insertUnconditionalBranch(*OrigBB, NewBB, DebugLoc());

  // Insert an entry into BlockInfo to align it properly with the block numbers.
  BlockInfo.insert(BlockInfo.begin() + NewBB->getNumber(), BasicBlockInfo());

  NewBB->transferSuccessors(OrigBB);
  OrigBB->addSuccessor(NewBB);
  OrigBB->addSuccessor(DestBB);

  // Cleanup potential unconditional branch to successor block.
  // Note that updateTerminator may change the size of the blocks.
  OrigBB->updateTerminator(NewBB);

  // Figure out how large the OrigBB is.  As the first half of the original
  // block, it cannot contain a tablejump.  The size includes
  // the new jump we added.  (It should be possible to do this without
  // recounting everything, but it's very confusing, and this is rarely
  // executed.)
  BlockInfo[OrigBB->getNumber()].Size = computeBlockSize(*OrigBB);

  // Figure out how large the NewMBB is. As the second half of the original
  // block, it may contain a tablejump.
  BlockInfo[NewBB->getNumber()].Size = computeBlockSize(*NewBB);

  // Update the offset of the new block.
  adjustBlockOffsets(*OrigBB, std::next(NewBB->getIterator()));

  // Need to fix live-in lists if we track liveness.
  if (TRI->trackLivenessAfterRegAlloc(*MF))
    computeAndAddLiveIns(LiveRegs, *NewBB);

  ++RISCVRelayoutNumSplit;

  return NewBB;
}

/// isBlockInRange - Returns true if the distance between specific MI and
/// specific BB can fit in MI's displacement field.
bool RISCVRelayoutImpl::isBlockInRange(const MachineInstr &MI,
                                      const MachineBasicBlock &DestBB) const {
  int64_t BrOffset = getInstrOffset(MI);
  int64_t DestOffset = BlockInfo[DestBB.getNumber()].Offset;

  const MachineBasicBlock *SrcBB = MI.getParent();

  if (TII->isBranchOffsetInRange(MI.getOpcode(),
                                 SrcBB->getSectionID() != DestBB.getSectionID()
                                     ? TM->getMaxCodeSize()
                                     : DestOffset - BrOffset))
    return true;

  LLVM_DEBUG(dbgs() << "Out of range branch to destination "
                    << printMBBReference(DestBB) << " from "
                    << printMBBReference(*MI.getParent()) << " to "
                    << DestOffset << " offset " << DestOffset - BrOffset << '\t'
                    << MI);

  return false;
}

/// wrapInVLIWBundle - Wrap non-debug instructions in NewBB into legal
/// 8-slot VLIW bundles, filling empty slots with NOPs.
/// No-ops when itinerary data is absent (non-Dandelion target).
void RISCVRelayoutImpl::wrapInVLIWBundle(MachineBasicBlock &NewBB) {
  if (!RII || !IID || IID->isEmpty())
    return;

  SmallVector<MachineInstr *, DandelionSlotsRL> Instrs;
  for (MachineInstr &MI : NewBB)
    if (!MI.isDebugInstr())
      Instrs.push_back(&MI);
  if (Instrs.empty())
    return;

  SmallVector<RISCVVLIW::SlotPacket, 4> Packets;
  RISCVVLIW::partitionIntoPackets(Instrs, IID, TRI, Packets);

  MachineBasicBlock::instr_iterator InsertPt = NewBB.instr_end();
  for (MachineInstr *MI : Instrs)
    MI->removeFromParent();

  for (const RISCVVLIW::SlotPacket &Packet : Packets) {
    std::array<MachineInstr *, RISCVVLIW::DandelionSlots> SlotInstr;
    SlotInstr.fill(nullptr);
    for (unsigned I = 0, E = Packet.Instrs.size(); I != E; ++I) {
      assert(Packet.Slots[I] >= 0 &&
             Packet.Slots[I] < (int)RISCVVLIW::DandelionSlots);
      SlotInstr[Packet.Slots[I]] = Packet.Instrs[I];
    }

    MachineBasicBlock::instr_iterator FirstInserted;
    bool FirstSet = false;
    for (unsigned Slot = 0; Slot < RISCVVLIW::DandelionSlots; ++Slot) {
      MachineInstr *MI;
      if (SlotInstr[Slot]) {
        MI = SlotInstr[Slot];
        NewBB.insert(InsertPt, MI);
      } else if (Slot == 0) {
        MI = BuildMI(NewBB, InsertPt, DebugLoc(), RII->get(RISCV::FEQ_S))
                 .addDef(RISCV::X0)
                 .addReg(RISCV::F0_F, RegState::Undef)
                 .addReg(RISCV::F0_F, RegState::Undef);
      } else {
        MI = BuildMI(NewBB, InsertPt, DebugLoc(), RII->get(RISCV::ADDI))
                 .addDef(RISCV::X0)
                 .addReg(RISCV::X0)
                 .addImm(0);
      }
      if (!FirstSet) {
        FirstInserted = MI->getIterator();
        FirstSet = true;
      }
    }

    assert(FirstSet && "wrapInVLIWBundle: no instructions were inserted");
    RISCVVLIW::finalizeRISCVBundle(NewBB, FirstInserted, InsertPt);
  }

  BlockInfo[NewBB.getNumber()].Size = computeBlockSize(NewBB);
}

/// wrapStandaloneBranchesInMBB - Wrap each standalone (non-bundled) branch
/// instruction at the tail of MBB into its own 8-slot VLIW bundle:
///   SLOT0: FEQ.S x0, f0, f0  (FDivFPU NOP)
///   SLOT1-6: ADDI x0, x0, 0  (ALU NOP)
///   SLOT7: the branch instruction
/// Called after TII->insertBranch() adds new terminator(s) to an existing block.
void RISCVRelayoutImpl::wrapStandaloneBranchesInMBB(MachineBasicBlock &MBB) {
  if (!RII || !IID || IID->isEmpty())
    return;

  // Collect standalone (non-bundled) branch instructions at the end of MBB.
  SmallVector<MachineInstr *, 4> Branches;
  for (auto I = MBB.instr_rbegin(); I != MBB.instr_rend(); ++I) {
    if (I->isDebugInstr())
      continue;
    if (I->isInsideBundle() || I->isBundle())
      break;
    if (!I->isBranch())
      break;
    Branches.push_back(&*I);
  }
  if (Branches.empty())
    return;

  // Process in forward (program) order so bundles are emitted in order.
  std::reverse(Branches.begin(), Branches.end());

  for (MachineInstr *BrMI : Branches) {
    // Each branch gets its own 8-slot bundle with the branch at SLOT7.
    MachineBasicBlock::instr_iterator InsertPt =
        std::next(BrMI->getIterator());

    BrMI->removeFromParent();

    // SLOT0: FEQ.S x0, f0, f0
    MachineInstr *S0 = BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::FEQ_S))
                           .addDef(RISCV::X0)
                           .addReg(RISCV::F0_F, RegState::Undef)
                           .addReg(RISCV::F0_F, RegState::Undef);
    auto FirstInstr = S0->getIterator();

    // SLOT1-6: ADDI x0, x0, 0
    for (unsigned S = 1; S < 7; ++S)
      BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::ADDI))
          .addDef(RISCV::X0)
          .addReg(RISCV::X0)
          .addImm(0);

    // SLOT7: branch
    MBB.insert(InsertPt, BrMI);

    RISCVVLIW::finalizeRISCVBundle(MBB, FirstInstr, InsertPt);
  }

  // Recompute block size (each 4-byte branch is now inside a 32-byte bundle).
  BlockInfo[MBB.getNumber()].Size = computeBlockSize(MBB);
}

/// fixupConditionalBranch - Fix up a conditional branch whose destination is
/// too far away to fit in its displacement field. It is converted to an inverse
/// conditional branch + an unconditional branch to the destination.
bool RISCVRelayoutImpl::fixupConditionalBranch(MachineInstr &MI) {
  DebugLoc DL = MI.getDebugLoc();
  MachineBasicBlock *MBB = MI.getParent();
  MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
  MachineBasicBlock *NewBB = nullptr;
  SmallVector<MachineOperand, 4> Cond;

  auto insertUncondBranch = [&](MachineBasicBlock *MBB,
                                MachineBasicBlock *DestBB) {
    unsigned &BBSize = BlockInfo[MBB->getNumber()].Size;
    int NewBrSize = 0;
    TII->insertUnconditionalBranch(*MBB, DestBB, DL, &NewBrSize);
    BBSize += NewBrSize;
  };
  auto insertBranch = [&](MachineBasicBlock *MBB, MachineBasicBlock *TBB,
                          MachineBasicBlock *FBB,
                          SmallVectorImpl<MachineOperand> &Cond) {
    unsigned &BBSize = BlockInfo[MBB->getNumber()].Size;
    int NewBrSize = 0;
    TII->insertBranch(*MBB, TBB, FBB, Cond, DL, &NewBrSize);
    BBSize += NewBrSize;
  };
  // Bundle-aware branch removal: uses bundleAwareRemoveBranch so that branches
  // residing inside BUNDLE containers are found and erased correctly.
  auto removeBranch = [&](MachineBasicBlock *MBB) {
    unsigned &BBSize = BlockInfo[MBB->getNumber()].Size;
    bundleAwareRemoveBranch(*MBB, BBSize);
  };

  // Populate the block offset and live-ins for a new basic block.
  auto updateOffsetAndLiveness = [&](MachineBasicBlock *NewBB) {
    assert(NewBB != nullptr && "can't populate offset for nullptr");

    // Keep the block offsets approximately up to date. While they will be
    // slight underestimates, we will update them appropriately in the next
    // scan through the function.
    adjustBlockOffsets(*std::prev(NewBB->getIterator()),
                       std::next(NewBB->getIterator()));

    // Need to fix live-in lists if we track liveness.
    if (TRI->trackLivenessAfterRegAlloc(*MF))
      computeAndAddLiveIns(LiveRegs, *NewBB);
  };

  // Use the bundle-aware analyzer so that branches inside BUNDLE containers
  // are visible (TII->analyzeBranch uses getDesc() which is blind to bundles).
  bool Fail = bundleAwareAnalyzeBranch(*MBB, TBB, FBB, Cond);
  assert(!Fail && "branches to be relaxed must be analyzable");
  (void)Fail;

  // Since cross-section conditional branches to the cold section are rarely
  // taken, try to avoid inverting the condition. Instead, add a "trampoline
  // branch", which unconditionally branches to the branch destination. Place
  // the trampoline branch at the end of the function and retarget the
  // conditional branch to the trampoline.
  // tbz L1
  // =>
  // tbz L1Trampoline
  // ...
  // L1Trampoline: b  L1
  if (MBB->getSectionID() != TBB->getSectionID() &&
      TBB->getSectionID() == MBBSectionID::ColdSectionID &&
      TrampolineInsertionPoint != nullptr) {
    // If the insertion point is out of range, we can't put a trampoline there.
    NewBB =
        createNewBlockAfter(*TrampolineInsertionPoint, MBB->getBasicBlock());

    if (isBlockInRange(MI, *NewBB)) {
      LLVM_DEBUG(dbgs() << "  Retarget destination to trampoline at "
                        << NewBB->back());

      insertUncondBranch(NewBB, TBB);

      // Update the successor lists to include the trampoline.
      MBB->replaceSuccessor(TBB, NewBB);
      NewBB->addSuccessor(TBB);

      // Replace branch in the current (MBB) block.
      removeBranch(MBB);
      insertBranch(MBB, NewBB, FBB, Cond);
      wrapStandaloneBranchesInMBB(*MBB);

      TrampolineInsertionPoint = NewBB;
      wrapInVLIWBundle(*NewBB);
      updateOffsetAndLiveness(NewBB);
      return true;
    }

    LLVM_DEBUG(
        dbgs() << "  Trampoline insertion point out of range for Bcc from "
               << printMBBReference(*MBB) << " to " << printMBBReference(*TBB)
               << ".\n");
    TrampolineInsertionPoint->setIsEndSection(NewBB->isEndSection());
    MF->erase(NewBB);
    NewBB = nullptr;
  }

  // Add an unconditional branch to the destination and invert the branch
  // condition to jump over it:
  // tbz L1
  // =>
  // tbnz L2
  // b   L1
  // L2:

  bool ReversedCond = !TII->reverseBranchCondition(Cond);
  if (ReversedCond) {
    if (FBB && isBlockInRange(MI, *FBB)) {
      // Last MI in the BB is an unconditional branch. We can simply invert the
      // condition and swap destinations:
      // beq L1
      // b   L2
      // =>
      // bne L2
      // b   L1
      LLVM_DEBUG(dbgs() << "  Invert condition and swap "
                           "its destination with "
                        << MBB->back());

      removeBranch(MBB);
      insertBranch(MBB, FBB, TBB, Cond);
      wrapStandaloneBranchesInMBB(*MBB);
      return true;
    }
    if (FBB) {
      // We need to split the basic block here to obtain two long-range
      // unconditional branches.
      NewBB = createNewBlockAfter(*MBB);

      insertUncondBranch(NewBB, FBB);
      // Update the succesor lists according to the transformation to follow.
      // Do it here since if there's no split, no update is needed.
      MBB->replaceSuccessor(FBB, NewBB);
      NewBB->addSuccessor(FBB);
      wrapInVLIWBundle(*NewBB);
      updateOffsetAndLiveness(NewBB);
    }

    // We now have an appropriate fall-through block in place (either naturally
    // or just created), so we can use the inverted the condition.
    MachineBasicBlock &NextBB = *std::next(MachineFunction::iterator(MBB));

    LLVM_DEBUG(dbgs() << "  Insert B to " << printMBBReference(*TBB)
                      << ", invert condition and change dest. to "
                      << printMBBReference(NextBB) << '\n');

    removeBranch(MBB);
    // Insert a new conditional branch and a new unconditional branch.
    insertBranch(MBB, &NextBB, TBB, Cond);
    wrapStandaloneBranchesInMBB(*MBB);
    // Force the label for NextBB: after UnpackMachineBundles the VLIW NOPs
    // from the branch bundle will be interleaved between the two terminators,
    // so AsmPrinter::getFirstTerminator() stops at those NOPs and never sees
    // the conditional branch to NextBB. Setting LabelMustBeEmitted ensures
    // the label is always emitted regardless of fall-through analysis.
    NextBB.setLabelMustBeEmitted();
    return true;
  }
  // Branch cond can't be inverted.
  // In this case we always add a block after the MBB.
  LLVM_DEBUG(dbgs() << "  The branch condition can't be inverted. "
                    << "  Insert a new BB after " << MBB->back());

  if (!FBB)
    FBB = &(*std::next(MachineFunction::iterator(MBB)));

  // This is the block with cond. branch and the distance to TBB is too long.
  //    beq L1
  // L2:

  // We do the following transformation:
  //    beq NewBB
  //    b L2
  // NewBB:
  //    b L1
  // L2:

  NewBB = createNewBlockAfter(*MBB);
  insertUncondBranch(NewBB, TBB);

  LLVM_DEBUG(dbgs() << "  Insert cond B to the new BB "
                    << printMBBReference(*NewBB)
                    << "  Keep the exiting condition.\n"
                    << "  Insert B to " << printMBBReference(*FBB) << ".\n"
                    << "  In the new BB: Insert B to "
                    << printMBBReference(*TBB) << ".\n");

  // Update the successor lists according to the transformation to follow.
  MBB->replaceSuccessor(TBB, NewBB);
  NewBB->addSuccessor(TBB);

  // Replace branch in the current (MBB) block.
  removeBranch(MBB);
  insertBranch(MBB, NewBB, FBB, Cond);
  wrapStandaloneBranchesInMBB(*MBB);

  wrapInVLIWBundle(*NewBB);
  updateOffsetAndLiveness(NewBB);
  return true;
}

bool RISCVRelayoutImpl::fixupUnconditionalBranch(MachineInstr &MI) {
  MachineBasicBlock *MBB = MI.getParent();
  unsigned OldBrSize = TII->getInstSizeInBytes(MI);
  MachineBasicBlock *DestBB = TII->getBranchDestBlock(MI);

  int64_t DestOffset = BlockInfo[DestBB->getNumber()].Offset;
  int64_t SrcOffset = getInstrOffset(MI);

  assert(!TII->isBranchOffsetInRange(
      MI.getOpcode(), MBB->getSectionID() != DestBB->getSectionID()
                          ? TM->getMaxCodeSize()
                          : DestOffset - SrcOffset));

  BlockInfo[MBB->getNumber()].Size -= OldBrSize;

  MachineBasicBlock *BranchBB = MBB;

  // If this was an expanded conditional branch, there is already a single
  // unconditional branch in a block.
  if (!MBB->empty()) {
    BranchBB = createNewBlockAfter(*MBB);

    // Add live outs.
    for (const MachineBasicBlock *Succ : MBB->successors()) {
      for (const MachineBasicBlock::RegisterMaskPair &LiveIn : Succ->liveins())
        BranchBB->addLiveIn(LiveIn);
    }

    BranchBB->sortUniqueLiveIns();
    BranchBB->addSuccessor(DestBB);
    MBB->replaceSuccessor(DestBB, BranchBB);
    if (TrampolineInsertionPoint == MBB)
      TrampolineInsertionPoint = BranchBB;
  }

  DebugLoc DL = MI.getDebugLoc();
  MI.eraseFromParent();

  // Create the optional restore block and, initially, place it at the end of
  // function. That block will be placed later if it's used; otherwise, it will
  // be erased.
  MachineBasicBlock *RestoreBB =
      createNewBlockAfter(MF->back(), DestBB->getBasicBlock());
  std::prev(RestoreBB->getIterator())
      ->setIsEndSection(RestoreBB->isEndSection());
  RestoreBB->setIsEndSection(false);

  TII->insertIndirectBranch(*BranchBB, *DestBB, *RestoreBB, DL,
                            BranchBB->getSectionID() != DestBB->getSectionID()
                                ? TM->getMaxCodeSize()
                                : DestOffset - SrcOffset,
                            RS.get());

  // Wrap the newly created block in a VLIW bundle before updating sizes.
  wrapInVLIWBundle(*BranchBB);

  // Update the block size and offset for the BranchBB (which may be newly
  // created).
  BlockInfo[BranchBB->getNumber()].Size = computeBlockSize(*BranchBB);
  adjustBlockOffsets(*MBB, std::next(BranchBB->getIterator()));

  // If RestoreBB is required, place it appropriately.
  if (!RestoreBB->empty()) {
    // If the jump is Cold -> Hot, don't place the restore block (which is
    // cold) in the middle of the function. Place it at the end.
    if (MBB->getSectionID() == MBBSectionID::ColdSectionID &&
        DestBB->getSectionID() != MBBSectionID::ColdSectionID) {
      MachineBasicBlock *NewBB = createNewBlockAfter(*TrampolineInsertionPoint);
      TII->insertUnconditionalBranch(*NewBB, DestBB, DebugLoc());
      wrapInVLIWBundle(*NewBB);
      BlockInfo[NewBB->getNumber()].Size = computeBlockSize(*NewBB);
      adjustBlockOffsets(*TrampolineInsertionPoint,
                         std::next(NewBB->getIterator()));

      // New trampolines should be inserted after NewBB.
      TrampolineInsertionPoint = NewBB;

      // Retarget the unconditional branch to the trampoline block.
      BranchBB->replaceSuccessor(DestBB, NewBB);
      NewBB->addSuccessor(DestBB);

      DestBB = NewBB;
    }

    // In all other cases, try to place just before DestBB.

    // TODO: For multiple far branches to the same destination, there are
    // chances that some restore blocks could be shared if they clobber the
    // same registers and share the same restore sequence. So far, those
    // restore blocks are just duplicated for each far branch.
    assert(!DestBB->isEntryBlock());
    MachineBasicBlock *PrevBB = &*std::prev(DestBB->getIterator());
    // Fall through only if PrevBB has no unconditional branch as one of its
    // terminators.
    if (auto *FT = PrevBB->getLogicalFallThrough()) {
      assert(FT == DestBB);
      TII->insertUnconditionalBranch(*PrevBB, FT, DebugLoc());
      BlockInfo[PrevBB->getNumber()].Size = computeBlockSize(*PrevBB);
    }
    // Now, RestoreBB could be placed directly before DestBB.
    MF->splice(DestBB->getIterator(), RestoreBB->getIterator());
    // Update successors and predecessors.
    RestoreBB->addSuccessor(DestBB);
    BranchBB->replaceSuccessor(DestBB, RestoreBB);
    if (TRI->trackLivenessAfterRegAlloc(*MF))
      computeAndAddLiveIns(LiveRegs, *RestoreBB);
    // Wrap RestoreBB in a VLIW bundle before computing its size.
    wrapInVLIWBundle(*RestoreBB);
    // Compute the restore block size.
    BlockInfo[RestoreBB->getNumber()].Size = computeBlockSize(*RestoreBB);
    // Update the estimated offset for the restore block.
    adjustBlockOffsets(*PrevBB, DestBB->getIterator());

    // Fix up section information for RestoreBB and DestBB
    RestoreBB->setSectionID(DestBB->getSectionID());
    RestoreBB->setIsBeginSection(DestBB->isBeginSection());
    DestBB->setIsBeginSection(false);
    RelaxedUnconditionals.insert({BranchBB, RestoreBB});
  } else {
    // Remove restore block if it's not required.
    MF->erase(RestoreBB);
    RelaxedUnconditionals.insert({BranchBB, DestBB});
  }

  return true;
}

// Bundle-aware replacement for TII->analyzeBranch.
// Uses getFirstInstrTerminator() to penetrate BUNDLE containers and examine
// the actual branch instructions inside them.
bool RISCVRelayoutImpl::bundleAwareAnalyzeBranch(
    MachineBasicBlock &MBB, MachineBasicBlock *&TBB, MachineBasicBlock *&FBB,
    SmallVectorImpl<MachineOperand> &Cond) {
  TBB = FBB = nullptr;
  Cond.clear();

  // Collect actual branch instructions via block-level terminator scan.
  // For each block-level terminator (BUNDLE or standalone), getEffectiveBranchMI
  // extracts the real branch instruction; non-branch terminators are skipped.
  SmallVector<MachineInstr *, 2> Branches;
  for (auto J = MBB.getFirstTerminator(), E = MBB.end(); J != E; ++J) {
    MachineInstr *BrMI = getEffectiveBranchMI(*J);
    if (!BrMI)
      continue;
    if (BrMI->isPreISelOpcode())
      return true;
    if (BrMI->getDesc().isIndirectBranch())
      return true;
    Branches.push_back(BrMI);
    if (Branches.size() > 2)
      return true;
  }

  if (Branches.empty())
    return false;

  MachineInstr *Last = Branches.back();

  if (Branches.size() == 1) {
    if (Last->getDesc().isUnconditionalBranch()) {
      TBB = TII->getBranchDestBlock(*Last);
      return false;
    }
    if (Last->getDesc().isConditionalBranch()) {
      TBB = TII->getBranchDestBlock(*Last);
      // Replicate parseCondBranch: {opcode, rs1, rs2}
      Cond.push_back(MachineOperand::CreateImm(Last->getOpcode()));
      Cond.push_back(Last->getOperand(0));
      Cond.push_back(Last->getOperand(1));
      return false;
    }
    return true;
  }

  // Two branches: must be conditional + unconditional.
  MachineInstr *Prev = Branches[0];
  if (Prev->getDesc().isConditionalBranch() &&
      Last->getDesc().isUnconditionalBranch()) {
    TBB = TII->getBranchDestBlock(*Prev);
    Cond.push_back(MachineOperand::CreateImm(Prev->getOpcode()));
    Cond.push_back(Prev->getOperand(0));
    Cond.push_back(Prev->getOperand(1));
    FBB = TII->getBranchDestBlock(*Last);
    return false;
  }
  return true;
}

// Bundle-aware replacement for TII->removeBranch.
// Walks backward with instr_iterator, erasing branch members from their
// bundles (or standalone branches directly).  Updates BBSize to reflect the
// removed bytes so BlockInfo stays consistent.
// After erasing a branch from a VLIW bundle:
//   - if all remaining members are NOPs → delete the entire bundle
//   - otherwise → insert an ADDI NOP to restore the 8-slot count
unsigned RISCVRelayoutImpl::bundleAwareRemoveBranch(MachineBasicBlock &MBB,
                                                     unsigned &BBSize) {
  unsigned Count = 0;
  // Forward scan over block-level terminators; getEffectiveBranchMI extracts
  // the real branch from each BUNDLE (or standalone instruction).
  for (auto I = MBB.getFirstTerminator(); I != MBB.end(); ) {
    auto Next = std::next(I);
    if (MachineInstr *BrMI = getEffectiveBranchMI(*I)) {
      // Save the owning bundle header before erasing.
      MachineInstr *BundleHdr = nullptr;
      if (BrMI->isInsideBundle()) {
        auto It = BrMI->getIterator();
        while (It->isInsideBundle())
          --It;
        BundleHdr = &*It;
      }

      BBSize -= TII->getInstSizeInBytes(*BrMI); // -4
      BrMI->eraseFromBundle();
      ++Count;

      // Post-removal bundle cleanup (VLIW-specific).
      if (BundleHdr && RII && IID && !IID->isEmpty()) {
        SmallVector<MachineInstr *, DandelionSlotsRL> Members;
        bool AllNop = true;
        for (auto Mit = std::next(BundleHdr->getIterator());
             Mit != MBB.instr_end() && Mit->isInsideBundle(); ++Mit) {
          Members.push_back(&*Mit);
          if (!isNopMI(*Mit))
            AllNop = false;
        }

        if (AllNop) {
          // All remaining slots are NOPs → delete the entire bundle.
          // eraseFromParent() on a BUNDLE header erases the header + all members.
          BBSize -= (unsigned)(Members.size() * 4);
          BundleHdr->eraseFromParent();
        } else {
          // Real instructions remain → restore SLOT7 with an ADDI NOP.
          auto InsertPt = std::next(Members.back()->getIterator());
          BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::ADDI))
              .addDef(RISCV::X0)
              .addReg(RISCV::X0)
              .addImm(0)
              ->bundleWithPred();
          BBSize += 4; // NOP added; net change versus before call = 0
        }
      }
    }
    I = Next;
  }
  return Count;
}

bool RISCVRelayoutImpl::relaxBranchInstructions() {
  bool Changed = false;

  // Relaxing branches involves creating new basic blocks, so re-eval
  // end() for termination.
  for (MachineBasicBlock &MBB : *MF) {
    // Empty block?
    MachineBasicBlock::iterator Last = MBB.getLastNonDebugInstr();
    if (Last == MBB.end())
      continue;

    // Expand the unconditional branch first if necessary. If there is a
    // conditional branch, this will end up changing the branch destination of
    // it to be over the newly inserted indirect branch block, which may avoid
    // the need to try expanding the conditional branch first, saving an extra
    // jump.
    // Extract the actual branch from the last instruction (may be a BUNDLE).
    if (MachineInstr *BrMI = getEffectiveBranchMI(*Last)) {
      if (BrMI->isUnconditionalBranch()) {
        // Unconditional branch destination might be unanalyzable, assume these
        // are OK.
        if (MachineBasicBlock *DestBB = TII->getBranchDestBlock(*BrMI)) {
          if (!isBlockInRange(*BrMI, *DestBB) && !TII->isTailCall(*BrMI) &&
              !RelaxedUnconditionals.contains({&MBB, DestBB})) {
            fixupUnconditionalBranch(*BrMI);
            ++RISCVRelayoutNumUnconditionalRelaxed;
            Changed = true;
          }
        }
      }
    }

    // Loop over the conditional branches.
    // Use getFirstInstrTerminator() so that branches inside BUNDLEs are found.
    for (auto J = MBB.getFirstInstrTerminator(); J != MBB.instr_end(); ) {
      auto Next = std::next(J);

      // Skip BUNDLE headers and debug instructions — we want the real branch.
      if (J->isBundle() || J->isDebugInstr()) { J = Next; continue; }
      if (!J->isTerminator())                  { J = Next; continue; }
      if (!J->isConditionalBranch())           { J = Next; continue; }

      MachineInstr &MI = *J;

      if (MI.getOpcode() == TargetOpcode::FAULTING_OP) {
        // FAULTING_OP's destination is not encoded in the instruction stream
        // and thus never needs relaxed.
        J = Next;
        continue;
      }

      MachineBasicBlock *DestBB = TII->getBranchDestBlock(MI);
      if (!isBlockInRange(MI, *DestBB)) {
        // Check whether there is a second conditional branch ahead (possibly
        // inside another bundle).
        auto NextCond = Next;
        while (NextCond != MBB.instr_end() &&
               (NextCond->isDebugInstr() || NextCond->isBundle()))
          ++NextCond;

        if (NextCond != MBB.instr_end() && NextCond->isConditionalBranch()) {
          // Multiple conditional branches: split before the bundle (or
          // instruction) that owns the second conditional branch.
          MachineInstr *SplitMI = &*NextCond;
          while (SplitMI->isInsideBundle()) {
            auto Prev = SplitMI->getIterator();
            --Prev;
            SplitMI = &*Prev;
          }
          splitBlockBeforeInstr(*SplitMI, DestBB);
        } else {
          fixupConditionalBranch(MI);
          ++RISCVRelayoutNumConditionalRelaxed;
        }

        Changed = true;

        // This may have modified all of the terminators, so start over.
        Next = MBB.getFirstInstrTerminator();
      }
      J = Next;
    }
  }

  // If we relaxed a branch, we must recompute offsets for *all* basic blocks.
  // Otherwise, we may underestimate branch distances and fail to relax a branch
  // that has been pushed out of range.
  if (Changed)
    adjustBlockOffsets(MF->front());

  return Changed;
}

bool RISCVRelayoutImpl::run(MachineFunction &mf) {
  MF = &mf;

  LLVM_DEBUG(dbgs() << "***** RISCVRelayout *****\n");

  const TargetSubtargetInfo &ST = MF->getSubtarget();
  TII = ST.getInstrInfo();
  TM = &MF->getTarget();

  TRI = ST.getRegisterInfo();

  // Initialize RISCV-specific pointers for VLIW bundle construction.
  // wrapInVLIWBundle is a no-op when IID is absent (non-Dandelion targets).
  const RISCVSubtarget &RST = MF->getSubtarget<RISCVSubtarget>();
  RII = RST.getInstrInfo();
  IID = RST.getInstrItineraryData();
  // This is the bundle-aware branch-range pass. The generic RISC-V branch
  // relaxation pass has already run, and touching block numbering here would
  // perturb non-Dandelion output even when no VLIW work is possible.
  if (!IID || IID->isEmpty())
    return false;

  if (TRI->trackLivenessAfterRegAlloc(*MF))
    RS.reset(new RegScavenger());

  // Renumber all of the machine basic blocks in the function, guaranteeing that
  // the numbers agree with the position of the block in the function.
  MF->RenumberBlocks();

  // Do the initial scan of the function, building up information about the
  // sizes of each block.
  scanFunction();

  LLVM_DEBUG(dbgs() << "  Basic blocks before relaxation\n"; dumpBBs(););

  bool MadeChange = false;
  while (relaxBranchInstructions())
    MadeChange = true;

  // After a while, this might be made debug-only, but it is not expensive.
  verify();

  LLVM_DEBUG(dbgs() << "  Basic blocks after relaxation\n\n"; dumpBBs());

  BlockInfo.clear();
  RelaxedUnconditionals.clear();

  return MadeChange;
}

FunctionPass *llvm::createRISCVRelayoutPass() { return new RISCVRelayout(); }
