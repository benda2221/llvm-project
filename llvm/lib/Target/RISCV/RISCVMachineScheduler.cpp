//===-- RISCVMachineScheduler.cpp - RISC-V MI schedulers ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVMachineScheduler.h"
#include "RISCVVLIWSlotUtils.h"
#include "llvm/CodeGen/DFAPacketizer.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/CodeGen/VLIWMachineScheduler.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
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

class DandelionVLIWResourceModel final : public VLIWResourceModel {
  const InstrItineraryData *IID;
  SmallVector<SUnit *, RISCVVLIW::DandelionSlots> IssuingPacket;

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
    if (isNonIssuingInstruction(MI))
      return true;
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
    if (isNonIssuingInstruction(MI)) {
      Packet.push_back(SU);
      return false;
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
    return StartNewCycle;
  }
};

class DandelionConvergingVLIWScheduler final
    : public ConvergingVLIWScheduler {
  VLIWResourceModel *
  createVLIWResourceModel(const TargetSubtargetInfo &STI,
                          const TargetSchedModel *SchedModel) const override {
    return new DandelionVLIWResourceModel(STI, SchedModel);
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
