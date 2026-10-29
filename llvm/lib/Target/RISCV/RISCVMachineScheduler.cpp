//===-- RISCVMachineScheduler.cpp - RISC-V MI schedulers ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVMachineScheduler.h"
#include "RISCV.h"
#include "RISCVVLIWSlotUtils.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/CodeGen/VLIWMachineScheduler.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "machine-scheduler"

namespace {

static bool isNonIssuingInstruction(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
  case TargetOpcode::EXTRACT_SUBREG:
  case TargetOpcode::INSERT_SUBREG:
  case TargetOpcode::SUBREG_TO_REG:
  case TargetOpcode::REG_SEQUENCE:
  case TargetOpcode::IMPLICIT_DEF:
  case TargetOpcode::KILL:
  case TargetOpcode::CFI_INSTRUCTION:
  case TargetOpcode::EH_LABEL:
  case TargetOpcode::COPY:
  case TargetOpcode::INLINEASM:
  case TargetOpcode::INLINEASM_BR:
    return true;
  }
}

static bool isSoloInstruction(const MachineInstr &MI) {
  return MI.getOpcode() == RISCV::FENCE || MI.getOpcode() == RISCV::FENCE_I ||
         MI.getOpcode() == RISCV::FENCE_TSO;
}

class DandelionVLIWResourceModel final : public VLIWResourceModel {
  const InstrItineraryData *IID;
  SmallVector<SUnit *, RISCVVLIW::DandelionSlots> IssuingPacket;

  bool occupiesSlot7(const MachineInstr &MI) const {
    return RISCVVLIW::getSlotMask(MI.getDesc().getSchedClass(), IID) == 0x80;
  }

  bool hasLegalSlot(const MachineInstr &MI) const {
    return RISCVVLIW::getSlotMask(MI.getDesc().getSchedClass(), IID) != 0;
  }

public:
  DandelionVLIWResourceModel(const TargetSubtargetInfo &STI,
                             const TargetSchedModel *SchedModel)
      : VLIWResourceModel(STI, SchedModel),
        IID(STI.getInstrItineraryData()) {}

  void reset() override {
    VLIWResourceModel::reset();
    IssuingPacket.clear();
  }

  bool isResourceAvailable(SUnit *SU, bool IsTop) override {
    if (!SU || !SU->getInstr())
      return false;
    MachineInstr &MI = *SU->getInstr();
    if (MI.isInlineAsm())
      return IssuingPacket.empty();
    if (isNonIssuingInstruction(MI))
      return true;
    // Some pre-RA pseudos, notably PseudoMovAddr, expand into multiple real
    // instructions after register allocation and consequently have no slot in
    // the Dandelion itinerary.  Keep them out of tentative packets and model
    // them as conservative scheduling boundaries.  The post-RA packetizer
    // will see and place the expanded instructions normally.
    if (!hasLegalSlot(MI))
      return IssuingPacket.empty();
    if (isSoloInstruction(MI) && !IssuingPacket.empty())
      return false;
    if (IsTop && !IssuingPacket.empty() &&
        occupiesSlot7(*IssuingPacket.back()->getInstr()))
      return false;
    if (!IsTop && occupiesSlot7(MI) && !IssuingPacket.empty())
      return false;
    if (!ResourcesModel->canReserveResources(MI))
      return false;

    SmallVector<SUnit *, RISCVVLIW::DandelionSlots> Trial;
    Trial.reserve(IssuingPacket.size() + 1);
    if (!IsTop)
      Trial.push_back(SU);
    Trial.append(IssuingPacket.begin(), IssuingPacket.end());
    if (IsTop)
      Trial.push_back(SU);

    RISCVVLIW::PacketLegalityState State;
    bool Legal = RISCVVLIW::buildPacketLegalityState(
        Trial, IID, /*AllowShallowDependencies=*/true, State);
    LLVM_DEBUG(dbgs() << "Dandelion VLIW candidate opcode=" << MI.getOpcode()
                      << " packet-size=" << IssuingPacket.size()
                      << " legal=" << Legal << '\n');
    return Legal;
  }

  bool reserveResources(SUnit *SU, bool IsTop) override {
    if (!SU) {
      reset();
      ++TotalPackets;
      return false;
    }

    MachineInstr &MI = *SU->getInstr();
    if (MI.isInlineAsm()) {
      bool StartNewCycle = !IssuingPacket.empty();
      if (StartNewCycle) {
        reset();
        ++TotalPackets;
      }
      // Inline assembly is emitted outside a compiler-created packet, but it
      // is still a hard scheduling boundary in both directions.
      reset();
      return true;
    }
    if (isNonIssuingInstruction(MI)) {
      Packet.push_back(SU);
      return false;
    }
    if (!hasLegalSlot(MI)) {
      bool StartNewCycle = !IssuingPacket.empty();
      if (StartNewCycle) {
        reset();
        ++TotalPackets;
      }
      LLVM_DEBUG(dbgs() << "Dandelion VLIW opaque boundary opcode="
                        << MI.getOpcode() << " direction="
                        << (IsTop ? "top" : "bottom") << '\n');
      reset();
      return true;
    }

    bool StartNewCycle = !isResourceAvailable(SU, IsTop);
    if (StartNewCycle) {
      reset();
      ++TotalPackets;
    }
    if (!isResourceAvailable(SU, IsTop))
      report_fatal_error(
          "Dandelion instruction cannot start a legal pre-RA packet");

    ResourcesModel->reserveResources(MI);
    if (IsTop)
      IssuingPacket.push_back(SU);
    else
      IssuingPacket.insert(IssuingPacket.begin(), SU);
    Packet.push_back(SU);

    // A top-down slot-7 instruction terminates the packet it just joined. A
    // bottom-up slot-7 instruction stays at the end of the tentative packet,
    // allowing earlier instructions to fill lower slots before it. Fences are
    // always single-instruction packets in either direction.
    if (isSoloInstruction(MI) || (IsTop && occupiesSlot7(MI))) {
      LLVM_DEBUG(dbgs() << "Dandelion VLIW packet boundary opcode="
                        << MI.getOpcode() << " direction="
                        << (IsTop ? "top" : "bottom") << '\n');
      reset();
      ++TotalPackets;
      return true;
    }
    return StartNewCycle;
  }

  int getCandidateCost(SUnit *SU, bool IsTop) {
    if (!SU || !SU->getInstr() ||
        isNonIssuingInstruction(*SU->getInstr()) ||
        !isResourceAvailable(SU, IsTop))
      return 0;

    MachineInstr &MI = *SU->getInstr();
    uint8_t SlotMask = RISCVVLIW::getSlotMask(
        MI.getDesc().getSchedClass(), IID);
    unsigned Choices = llvm::popcount(SlotMask);
    int Cost = Choices < 4 ? 4 - Choices : 0;

    // Delay a slot-7 terminator until useful lower-slot work has accumulated.
    // The adjustment stays deliberately small relative to register-pressure
    // and critical-path priorities in ConvergingVLIWScheduler.
    if (SlotMask == 0x80)
      Cost += IssuingPacket.empty() ? -8 : 2 * IssuingPacket.size();
    else if (!IssuingPacket.empty())
      Cost += 1;
    return Cost;
  }
};

class DandelionConvergingVLIWScheduler final
    : public ConvergingVLIWScheduler {
  VLIWResourceModel *
  createVLIWResourceModel(const TargetSubtargetInfo &STI,
                          const TargetSchedModel *SchedModel) const override {
    return new DandelionVLIWResourceModel(STI, SchedModel);
  }

  int SchedulingCost(ReadyQueue &Q, SUnit *SU, SchedCandidate &Candidate,
                     RegPressureDelta &Delta, bool Verbose) override {
    int Cost = ConvergingVLIWScheduler::SchedulingCost(
        Q, SU, Candidate, Delta, Verbose);
    bool IsTop = Q.getID() == TopQID;
    auto *ResourceModel = static_cast<DandelionVLIWResourceModel *>(
        IsTop ? Top.ResourceModel : Bot.ResourceModel);
    int Bonus = ResourceModel->getCandidateCost(SU, IsTop);
    LLVM_DEBUG(if (Bonus) dbgs()
               << "Dandelion VLIW cost bonus=" << Bonus << '\n');
    return Cost + Bonus;
  }
};

static ScheduleDAGMILive *
createVLIWScheduler(MachineSchedContext *Context,
                    std::unique_ptr<MachineSchedStrategy> Strategy) {
  auto *DAG = new VLIWMachineScheduler(Context, std::move(Strategy));
  DAG->addMutation(createCopyConstrainDAGMutation(DAG->TII, DAG->TRI));
  return DAG;
}

} // namespace

ScheduleDAGMILive *
llvm::createRISCVVLIWMachineScheduler(MachineSchedContext *Context) {
  return createVLIWScheduler(
      Context, std::make_unique<ConvergingVLIWScheduler>());
}

ScheduleDAGMILive *
llvm::createRISCVDandelionMachineScheduler(MachineSchedContext *Context) {
  return createVLIWScheduler(
      Context, std::make_unique<DandelionConvergingVLIWScheduler>());
}
