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
//   1. Determines the slot(s) each constituent instruction may occupy using
//      the itinerary data.
//   2. Assigns each instruction to a concrete slot, respecting:
//        - Instructions must be placed at one of their legal slot positions.
//        - WAW-dependent instructions preserve their relative order (the
//          earlier-writing instruction gets the lower slot number); this is
//          guaranteed by assigning slots left-to-right from the already
//          WAW-safe input order produced by the packetizer.
//   3. Inserts NOP instructions into unoccupied slots so that every BUNDLE
//      contains exactly 8 instructions (one per slot 0-7):
//        - SLOT 0 (FDivFPUPipeline): FSGNJ_S ft0, ft0, ft0  (fp NOP)
//        - SLOT 1-7 (ALU pipelines): ADDI x0, x0, 0         (integer NOP)
//   4. Re-finalizes the BUNDLE with the new slot order.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-pack-padding"

static cl::opt<bool> DisablePackPadding(
    "riscv-disable-packetpadding", cl::Hidden,
    cl::desc("Disable the RISCV packet padding pass"), cl::init(false));

// Number of VLIW slots in the Dandelion processor.
static constexpr unsigned DandelionSlots = 8;

namespace {

/// Return a bitmask of the slots (bit N == slot N) that the instruction with
/// the given SchedClass may occupy, derived from the itinerary stage FuncUnits.
/// The FuncUnit bits are laid out in the same order as the FuncUnit records in
/// the ProcessorItineraries list:
///   bit 0 -> RISCV_SLOT0, bit 1 -> RISCV_SLOT1, ..., bit 7 -> RISCV_SLOT7
///
/// If the itinerary data is absent (non-Dandelion CPU) returns 0xFF so the
/// instruction is treated as legal in any slot.
static uint8_t getSlotMask(unsigned SchedClass,
                            const InstrItineraryData *IID) {
  if (!IID || IID->isEmpty())
    return 0xFF;
  const InstrStage *IS = IID->beginStage(SchedClass);
  const InstrStage *End = IID->endStage(SchedClass);
  uint8_t Mask = 0;
  for (; IS != End; ++IS)
    Mask |= static_cast<uint8_t>(IS->getUnits());
  return Mask;
}

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
  // Step 1: Collect the real instructions inside the bundle.
  // -------------------------------------------------------------------------
  SmallVector<MachineInstr *, 8> Instrs;
  for (MachineBasicBlock::instr_iterator It = std::next(Bundle.getIterator()),
                                         E = MBB.instr_end();
       It != E && It->isInsideBundle(); ++It)
    Instrs.push_back(&*It);

  if (Instrs.empty())
    return false;

  // -------------------------------------------------------------------------
  // Step 2: Compute the legal slot mask for each instruction.
  // -------------------------------------------------------------------------
  SmallVector<uint8_t, 8> SlotMasks(Instrs.size());
  for (unsigned i = 0, e = Instrs.size(); i != e; ++i)
    SlotMasks[i] = getSlotMask(Instrs[i]->getDesc().getSchedClass(), IID);

  // -------------------------------------------------------------------------
  // Step 3: Assign each instruction to a concrete slot.
  //
  // Greedy left-to-right: pick the lowest available slot that is legal for the
  // instruction.  Because the packetizer has already ordered instructions to
  // satisfy WAW constraints (earlier write has lower position), processing them
  // in order and always taking the lowest free slot preserves that ordering.
  // -------------------------------------------------------------------------
  uint8_t OccupiedSlots = 0;
  SmallVector<int, 8> AssignedSlot(Instrs.size(), -1);

  for (unsigned i = 0, e = Instrs.size(); i != e; ++i) {
    uint8_t Legal = SlotMasks[i] & ~OccupiedSlots;
    if (!Legal)
      Legal = ~OccupiedSlots & 0xFF; // fallback: any free slot
    int Slot = __builtin_ctz(Legal);
    AssignedSlot[i] = Slot;
    OccupiedSlots |= static_cast<uint8_t>(1u << Slot);

    LLVM_DEBUG(dbgs() << "  Slot " << Slot << " <- " << *Instrs[i]);
  }

  // -------------------------------------------------------------------------
  // Step 4: Build a slot-indexed array; nullptr means empty (needs a NOP).
  // -------------------------------------------------------------------------
  std::array<MachineInstr *, DandelionSlots> SlotInstr;
  SlotInstr.fill(nullptr);
  for (unsigned i = 0, e = Instrs.size(); i != e; ++i) {
    assert(AssignedSlot[i] >= 0 && AssignedSlot[i] < (int)DandelionSlots);
    SlotInstr[AssignedSlot[i]] = Instrs[i];
  }

  // -------------------------------------------------------------------------
  // Step 5: Unbundle.
  //
  // Clear BundledPred and BundledSucc flags directly on every instruction in
  // the bundle (including the BUNDLE header) so that we can safely insert and
  // move them afterwards.  We also clear InternalRead flags on operands,
  // mirroring what UnpackMachineBundles does.
  //
  // We avoid the unbundleFromPred()/unbundleFromSucc() helpers here because
  // they modify neighbour flags incrementally and require a specific calling
  // order that is easy to get wrong.  Direct flag manipulation is safe as long
  // as we erase the BUNDLE header before re-inserting any members.
  // -------------------------------------------------------------------------

  // Capture InsertPt BEFORE any removal: points to the instruction after the
  // last bundle member.  This is stable because it is not part of the bundle.
  MachineBasicBlock::instr_iterator InsertPt =
      std::next(Instrs.back()->getIterator());

  // Clear bundle flags on the BUNDLE header.
  Bundle.clearFlag(MachineInstr::BundledSucc);
  Bundle.clearFlag(MachineInstr::BundledPred);

  // Clear bundle flags and internal-read operand flags on all members.
  for (MachineInstr *MI : Instrs) {
    MI->clearFlag(MachineInstr::BundledPred);
    MI->clearFlag(MachineInstr::BundledSucc);
    for (MachineOperand &MO : MI->operands())
      if (MO.isReg() && MO.isInternalRead())
        MO.setIsInternalRead(false);
  }

  // Erase the BUNDLE header (flags already cleared, so erase is safe).
  Bundle.eraseFromParent();

  // Remove all members from the MBB.  InsertPt remains valid because it points
  // to an instruction that is neither the header nor any member.
  for (MachineInstr *MI : Instrs)
    MI->removeFromParent();

  // -------------------------------------------------------------------------
  // Step 6: Re-insert instructions in slot order; fill gaps with NOPs.
  //         InsertPt is the position right after where the bundle was.
  // -------------------------------------------------------------------------
  bool NopInserted = false;
  MachineBasicBlock::instr_iterator FirstInserted;
  bool FirstSet = false;

  for (unsigned Slot = 0; Slot < DandelionSlots; ++Slot) {
    MachineInstr *MI;
    if (SlotInstr[Slot]) {
      MI = SlotInstr[Slot];
      MBB.insert(InsertPt, MI);
      LLVM_DEBUG(dbgs() << "  Slot " << Slot << " <- " << *MI);
    } else if (Slot == 0) {
      // SLOT 0 is the FDivFPUPipeline; fill with a floating-point NOP.
      // FSGNJ_S ft0, ft0, ft0 copies ft0's sign+magnitude into ft0 itself
      // — a no-op that is legal on SLOT 0 and has no observable effect.
      // Source operands are marked undef so the verifier does not require
      // a prior definition of ft0.
      MI = BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::FSGNJ_S))
               .addDef(RISCV::F0_F)
               .addReg(RISCV::F0_F, RegState::Undef)
               .addReg(RISCV::F0_F, RegState::Undef);
      NopInserted = true;
      LLVM_DEBUG(dbgs() << "  Slot 0 <- FP NOP (fsgnj.s ft0, ft0, ft0)\n");
    } else {
      // SLOT 1-7 are ALU-capable pipelines; fill with an integer NOP.
      MI = BuildMI(MBB, InsertPt, DebugLoc(), RII->get(RISCV::ADDI))
               .addDef(RISCV::X0)
               .addReg(RISCV::X0)
               .addImm(0);
      NopInserted = true;
      LLVM_DEBUG(dbgs() << "  Slot " << Slot << " <- NOP\n");
    }
    if (!FirstSet) {
      FirstInserted = MI->getIterator();
      FirstSet = true;
    }
  }

  // -------------------------------------------------------------------------
  // Step 7: Re-finalize the bundle over the 8 freshly-ordered instructions.
  //         finalizeBundle(MBB, First, Last) creates a new BUNDLE header and
  //         sets all BundledPred/Succ flags correctly.
  // -------------------------------------------------------------------------
  assert(FirstSet && "No instructions inserted");
  finalizeBundle(MBB, FirstInserted, InsertPt);

  return NopInserted;
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
