//===-- RISCVPackPadding.cpp - RISCV packet padding pass ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RISCVPackPadding pass for the Dandelion VLIW
// processor.
//
// The pass runs after the VLIW packetizer.  For every BUNDLE instruction it:
//   1. Reads and validates the concrete slot assignment recorded by the
//      packetizer on the BUNDLE header.
//   2. Reorders packet members into the recorded slot order.
//   3. Inserts NOP instructions into unoccupied slots so that every BUNDLE
//      contains exactly 8 instructions (one per slot 0-7):
//        - SLOT 0 (FDivFPUPipeline): FEQ_S   x0, f0, f0      (fp-class filler)
//        - SLOT 1-7 (ALU pipelines): ADDI x0, x0, 0         (integer NOP)
//   4. Re-finalizes the BUNDLE with the new slot order.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "RISCVVLIWBundleUtils.h"
#include "RISCVVLIWSlotUtils.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <array>

using namespace llvm;

#define DEBUG_TYPE "riscv-pack-padding"

static cl::opt<bool> DisablePackPadding(
    "riscv-disable-packetpadding", cl::Hidden,
    cl::desc("Disable the RISCV packet padding pass"), cl::init(false));

namespace {

// ============================================================================
// Pass declaration
// ============================================================================

class RISCVPackPadding : public MachineFunctionPass {
public:
  static char ID;
  RISCVPackPadding() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "RISCV Packet Padding"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const RISCVInstrInfo *RII = nullptr;
  const InstrItineraryData *IID = nullptr;

  bool padBundle(MachineBasicBlock &MBB, MachineInstr &Bundle);
};

} // end anonymous namespace

char RISCVPackPadding::ID = 0;

INITIALIZE_PASS(RISCVPackPadding, "riscv-pack-padding", "RISCV Packet Padding",
                false, false)

// ============================================================================
// Pad a single BUNDLE instruction
// ============================================================================

bool RISCVPackPadding::padBundle(MachineBasicBlock &MBB, MachineInstr &Bundle) {
  assert(Bundle.isBundle() && "Expected a BUNDLE instruction");

  // -------------------------------------------------------------------------
  // Step 1: Collect instructions inside the bundle.
  //         Debug instructions are tracked separately and re-inserted outside
  //         the rebuilt bundle so they do not consume hardware slots.
  // -------------------------------------------------------------------------
  SmallVector<MachineInstr *, 8> BundleMembers;
  SmallVector<MachineInstr *, 8> Instrs;
  SmallVector<MachineInstr *, 4> DebugInstrs;
  SmallVector<MachineInstr *, 4> ImplicitDefs;
  for (MachineBasicBlock::instr_iterator It = std::next(Bundle.getIterator()),
                                         E = MBB.instr_end();
       It != E && It->isInsideBundle(); ++It) {
    MachineInstr *MI = &*It;
    BundleMembers.push_back(MI);
    if (MI->isDebugInstr())
      DebugInstrs.push_back(MI);
    else if (MI->isImplicitDef())
      ImplicitDefs.push_back(MI);
    else
      Instrs.push_back(MI);
  }

  if (BundleMembers.empty())
    return false;

  SmallVector<int, RISCVVLIW::DandelionSlots> AssignedSlots;
  if (!RISCVVLIW::decodeDandelionSlotMap(Bundle, Instrs.size(),
                                         AssignedSlots) ||
      !RISCVVLIW::verifyDandelionSlotMap(Bundle, Instrs, IID))
    report_fatal_error(
        "RISCVPackPadding: invalid or missing packetizer slot assignment");

  // -------------------------------------------------------------------------
  // Step 5: Unbundle.
  //
  // Clear BundledPred and BundledSucc flags directly on every instruction in
  // the bundle (including the BUNDLE header) so that we can safely insert and
  // move them afterwards.  InternalRead flags are intentionally preserved.
  //
  // We avoid the unbundleFromPred()/unbundleFromSucc() helpers here because
  // they modify neighbour flags incrementally and require a specific calling
  // order that is easy to get wrong.  Direct flag manipulation is safe as long
  // as we erase the BUNDLE header before re-inserting any members.
  // -------------------------------------------------------------------------

  // Capture InsertPt BEFORE any removal: points to the instruction after the
  // last bundle member.  This is stable because it is not part of the bundle.
  MachineBasicBlock::instr_iterator InsertPt =
      std::next(BundleMembers.back()->getIterator());

  // Clear bundle flags on the BUNDLE header.
  Bundle.clearFlag(MachineInstr::BundledSucc);
  Bundle.clearFlag(MachineInstr::BundledPred);

  // Clear bundle flags on all members.  InternalRead marks identify the one
  // permitted shallow RAW edge and must survive physical slot reordering.
  for (MachineInstr *MI : BundleMembers) {
    MI->clearFlag(MachineInstr::BundledPred);
    MI->clearFlag(MachineInstr::BundledSucc);
  }

  // Erase the BUNDLE header (flags already cleared, so erase is safe).
  Bundle.eraseFromParent();

  // Remove all members from the MBB.  InsertPt remains valid because it points
  // to an instruction that is neither the header nor any member.
  for (MachineInstr *MI : BundleMembers)
    MI->removeFromParent();

  // IMPLICIT_DEF is a non-issuing pseudo. Keep it in MIR for liveness/undef
  // semantics, but do not assign it a hardware VLIW slot.
  for (MachineInstr *IDI : ImplicitDefs)
    MBB.insert(InsertPt, IDI);

  // -------------------------------------------------------------------------
  // Step 6: Re-insert instructions in the slot order chosen by Packetizer and
  //         fill gaps with NOPs.  This pass deliberately does not solve slots
  //         or repartition an already formed packet.
  // -------------------------------------------------------------------------
  std::array<MachineInstr *, RISCVVLIW::DandelionSlots> SlotInstr;
  SlotInstr.fill(nullptr);
  for (unsigned I = 0, E = Instrs.size(); I != E; ++I) {
    assert(AssignedSlots[I] >= 0 &&
           AssignedSlots[I] < (int)RISCVVLIW::DandelionSlots);
    SlotInstr[AssignedSlots[I]] = Instrs[I];
  }

  MachineBasicBlock::instr_iterator FirstInserted;
  bool FirstSet = false;
  for (unsigned Slot = 0; Slot < RISCVVLIW::DandelionSlots; ++Slot) {
    MachineInstr *MI;
    if (SlotInstr[Slot]) {
      MI = SlotInstr[Slot];
      MBB.insert(InsertPt, MI);
      LLVM_DEBUG(dbgs() << "  Slot " << Slot << " <- " << *MI);
    } else if (Slot == 0) {
      MI = BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::FEQ_S))
               .addDef(RISCV::X0)
               .addReg(RISCV::F0_F, RegState::Undef)
               .addReg(RISCV::F0_F, RegState::Undef);
      LLVM_DEBUG(dbgs() << "  Slot 0 <- FP NOP (feq.s x0, f0, f0)\n");
    } else {
      MI = BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::ADDI))
               .addDef(RISCV::X0)
               .addReg(RISCV::X0)
               .addImm(0);
      LLVM_DEBUG(dbgs() << "  Slot " << Slot << " <- NOP\n");
    }
    if (!FirstSet) {
      FirstInserted = MI->getIterator();
      FirstSet = true;
    }
  }

  assert(FirstSet && "No instructions inserted");
  static constexpr std::array<int, RISCVVLIW::DandelionSlots>
      CanonicalSlots = {0, 1, 2, 3, 4, 5, 6, 7};
  RISCVVLIW::finalizeRISCVBundle(MBB, FirstInserted, InsertPt,
                                 CanonicalSlots);

  // Re-insert debug instructions after the rebuilt bundle. They should not
  // consume VLIW slots, but keeping them near the original location preserves
  // useful debug ordering.
  for (MachineInstr *DMI : DebugInstrs)
    MBB.insert(InsertPt, DMI);

  return true;
}

// ============================================================================
// runOnMachineFunction
// ============================================================================

bool RISCVPackPadding::runOnMachineFunction(MachineFunction &MF) {
  if (DisablePackPadding)
    return false;

  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  RII = ST.getInstrInfo();
  IID = ST.getInstrItineraryData();

  // Only operate when itinerary data is present (i.e. Dandelion CPU).
  if (!IID || IID->isEmpty())
    return false;

  LLVM_DEBUG(dbgs() << "RISCVPackPadding: processing " << MF.getName() << "\n");

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineBasicBlock::instr_iterator It = MBB.instr_begin(),
                                           E = MBB.instr_end();
         It != E;) {
      MachineInstr &MI = *It;
      ++It; // advance before potentially invalidating MI
      if (!MI.isBundle())
        continue;
      LLVM_DEBUG(dbgs() << "Padding bundle at " << MI << "\n");
      Changed |= padBundle(MBB, MI);
    }
  }
  return Changed;
}

// ============================================================================
// Public constructor
// ============================================================================

FunctionPass *llvm::createRISCVPackPaddingPass() {
  return new RISCVPackPadding();
}
