//===-- RISCVVLIWSlotUtils.cpp - Dandelion VLIW slot helpers -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVVLIWSlotUtils.h"
#include "RISCV.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <array>

using namespace llvm;

void RISCVVLIW::PacketLegalityState::clear() {
  SUnits.clear();
  ShallowProducer = nullptr;
  ShallowConsumer = nullptr;
  ShallowDataRegs.clear();
  MustPrecede = {};
  AssignedSlots.clear();
}

uint8_t RISCVVLIW::getSlotMask(unsigned SchedClass,
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

unsigned RISCVVLIW::getPreRAIssueSlotCount(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return 1;
  case RISCV::PseudoMovAddr:
  case RISCV::PseudoMovImm:
    return 2;
  }
}

static bool appendPreRAIssueSlotMasks(
    const MachineInstr &MI, const InstrItineraryData *IID,
    SmallVectorImpl<uint8_t> &SlotMasks) {
  uint8_t Mask = RISCVVLIW::getSlotMask(MI.getDesc().getSchedClass(), IID);
  if (!Mask)
    return false;
  SlotMasks.append(RISCVVLIW::getPreRAIssueSlotCount(MI), Mask);
  return true;
}

static bool assignSlotMasks(
    ArrayRef<uint8_t> SlotMasks,
    const RISCVVLIW::SlotOrderMatrix &MustPrecede,
    SmallVectorImpl<int> &AssignedSlots);

bool RISCVVLIW::isShallowDepOK(const MachineInstr &MI,
                               ShallowDepRole Role,
                               const InstrItineraryData *IID) {
  if (!IID || IID->isEmpty())
    return false;

  unsigned SchedClass = MI.getDesc().getSchedClass();
  std::optional<unsigned> DefCycle = IID->getOperandCycle(SchedClass, 0);
  const bool IsFastALU = getSlotMask(SchedClass, IID) == 0xFE &&
                         IID->getStageLatency(SchedClass) == 1 && DefCycle &&
                         *DefCycle == 2;
  if (Role == ShallowDepRole::Producer || IsFastALU)
    return IsFastALU;

  // Control-transfer consumers execute only in slot 7.  Identify them by
  // instruction semantics as well as timing: CSR instructions have the same
  // slot mask and similar itinerary cycles, but are intentionally excluded.
  const bool IsControlTransfer =
      MI.isBranch() || MI.isCall() || MI.isReturn() ||
      MI.getOpcode() == RISCV::JAL || MI.getOpcode() == RISCV::JALR;
  if (!IsControlTransfer || getSlotMask(SchedClass, IID) != 0x80 ||
      IID->getStageLatency(SchedClass) != 1 || !DefCycle || *DefCycle != 2)
    return false;

  // Dandelion's Branch/Jump itineraries put the first integer source at
  // operand cycle index 1.  Requiring the EX1 read cycle keeps this check tied
  // to the hardware path that receives a same-packet EX2 result.
  std::optional<unsigned> FirstUseCycle =
      IID->getOperandCycle(SchedClass, 1);
  return FirstUseCycle && *FirstUseCycle == 1;
}

bool RISCVVLIW::isShallowHardwareUseOperand(const MachineInstr &MI,
                                            const MachineOperand &MO,
                                            Register Reg) {
  if (!MO.isReg() || !MO.isUse() || MO.isUndef() || MO.getReg() != Reg)
    return false;
  if (!MO.isImplicit())
    return true;
  return MI.getOpcode() == RISCV::PseudoRET && Reg == RISCV::X1;
}

bool RISCVVLIW::hasShallowHardwareUse(const MachineInstr &MI, Register Reg) {
  for (const MachineOperand &MO : MI.operands())
    if (isShallowHardwareUseOperand(MI, MO, Reg))
      return true;
  return false;
}

bool RISCVVLIW::buildPacketLegalityState(
    ArrayRef<SUnit *> Units, const InstrItineraryData *IID,
    bool AllowShallowDependencies, PacketLegalityState &State) {
  State.clear();
  if (Units.empty() || Units.size() > DandelionSlots)
    return false;

  State.SUnits.append(Units.begin(), Units.end());
  DenseMap<SUnit *, unsigned> Indices;
  SmallVector<uint8_t, DandelionSlots> SlotMasks;
  SmallVector<unsigned, DandelionSlots> FirstAtoms;
  SmallVector<unsigned, DandelionSlots> DefAtoms;
  SlotOrderMatrix AtomMustPrecede{};
  for (unsigned I = 0; I != Units.size(); ++I)
    Indices[Units[I]] = I;

  // PseudoMovAddr and PseudoMovImm remain single MachineInstrs until after
  // register allocation, but each expands to two integer instructions. Model
  // them as two ordered issue atoms here so the pre-RA scheduler neither
  // treats them as packet boundaries nor under-counts their physical slots.
  for (SUnit *SU : Units) {
    if (!SU || !SU->getInstr())
      return false;
    unsigned FirstAtom = SlotMasks.size();
    if (!appendPreRAIssueSlotMasks(*SU->getInstr(), IID, SlotMasks) ||
        SlotMasks.size() > DandelionSlots)
      return false;
    unsigned DefAtom = SlotMasks.size() - 1;
    FirstAtoms.push_back(FirstAtom);
    DefAtoms.push_back(DefAtom);
    for (unsigned Atom = FirstAtom; Atom != DefAtom; ++Atom)
      AtomMustPrecede[Atom][Atom + 1] = true;
  }

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
        if (!AllowShallowDependencies || !Dep.isAssignedRegDep() ||
            !hasShallowHardwareUse(*Consumer->getInstr(), Dep.getReg()) ||
            !isShallowDepOK(*Producer->getInstr(),
                            ShallowDepRole::Producer, IID) ||
            !isShallowDepOK(*Consumer->getInstr(),
                            ShallowDepRole::Consumer, IID))
          return false;
        if (State.ShallowProducer &&
            (State.ShallowProducer != Producer ||
             State.ShallowConsumer != Consumer))
          return false;
        State.ShallowProducer = Producer;
        State.ShallowConsumer = Consumer;
        State.ShallowDataRegs.push_back(Dep.getReg());
        State.MustPrecede[I][J] = true;
        AtomMustPrecede[DefAtoms[I]][FirstAtoms[J]] = true;
        break;
      case SDep::Output:
        State.MustPrecede[I][J] = true;
        AtomMustPrecede[DefAtoms[I]][DefAtoms[J]] = true;
        break;
      case SDep::Anti:
        if (isShallowDepOK(*Consumer->getInstr(),
                           ShallowDepRole::Producer, IID) &&
            isShallowDepOK(*Producer->getInstr(),
                           ShallowDepRole::Consumer, IID)) {
          State.MustPrecede[I][J] = true;
          AtomMustPrecede[DefAtoms[I]][FirstAtoms[J]] = true;
        }
        break;
      case SDep::Order:
        if (!Dep.isWeak())
          return false;
        break;
      }
    }
  }

  SmallVector<int, DandelionSlots> AtomSlots;
  if (!assignSlotMasks(SlotMasks, AtomMustPrecede, AtomSlots))
    return false;
  for (unsigned DefAtom : DefAtoms)
    State.AssignedSlots.push_back(AtomSlots[DefAtom]);
  return true;
}

void RISCVVLIW::markShallowInternalReads(
    const PacketLegalityState &State) {
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

static bool satisfiesAssignedConstraints(
    unsigned InstrIdx, unsigned Slot, ArrayRef<int> AssignedSlots,
    const RISCVVLIW::SlotOrderMatrix &MustPrecede) {
  for (unsigned I = 0, E = AssignedSlots.size(); I != E; ++I) {
    if (AssignedSlots[I] < 0)
      continue;
    unsigned OtherSlot = AssignedSlots[I];
    if (MustPrecede[I][InstrIdx] && !(OtherSlot < Slot))
      return false;
    if (MustPrecede[InstrIdx][I] && !(Slot < OtherSlot))
      return false;
  }
  return true;
}

static bool assignSlotsRec(
    ArrayRef<uint8_t> SlotMasks,
    const RISCVVLIW::SlotOrderMatrix &MustPrecede,
    SmallVectorImpl<int> &AssignedSlots, uint8_t OccupiedSlots) {
  int BestInstr = -1;
  uint8_t BestCandidates = 0;
  unsigned BestCount = RISCVVLIW::DandelionSlots + 1;

  for (unsigned I = 0, E = SlotMasks.size(); I != E; ++I) {
    if (AssignedSlots[I] >= 0)
      continue;

    uint8_t Candidates = SlotMasks[I] & ~OccupiedSlots;
    for (unsigned S = 0; S < RISCVVLIW::DandelionSlots; ++S) {
      if (!(Candidates & (1u << S)))
        continue;
      if (!satisfiesAssignedConstraints(I, S, AssignedSlots, MustPrecede))
        Candidates &= static_cast<uint8_t>(~(1u << S));
    }

    unsigned Count = llvm::popcount(Candidates);
    if (!Count)
      return false;
    if (Count < BestCount) {
      BestInstr = I;
      BestCandidates = Candidates;
      BestCount = Count;
      if (Count == 1)
        break;
    }
  }

  if (BestInstr < 0)
    return true;

  for (unsigned S = 0; S < RISCVVLIW::DandelionSlots; ++S) {
    if (!(BestCandidates & (1u << S)))
      continue;
    AssignedSlots[BestInstr] = S;
    if (assignSlotsRec(SlotMasks, MustPrecede, AssignedSlots,
                       OccupiedSlots | static_cast<uint8_t>(1u << S)))
      return true;
    AssignedSlots[BestInstr] = -1;
  }

  return false;
}

static bool assignSlotMasks(
    ArrayRef<uint8_t> SlotMasks,
    const RISCVVLIW::SlotOrderMatrix &MustPrecede,
    SmallVectorImpl<int> &AssignedSlots) {
  AssignedSlots.assign(SlotMasks.size(), -1);
  if (SlotMasks.size() > RISCVVLIW::DandelionSlots)
    return false;
  for (uint8_t Mask : SlotMasks)
    if (!Mask)
      return false;
  return assignSlotsRec(SlotMasks, MustPrecede, AssignedSlots, 0);
}

bool RISCVVLIW::assignSlots(ArrayRef<MachineInstr *> Instrs,
                            const InstrItineraryData *IID,
                            const SlotOrderMatrix &MustPrecede,
                            SmallVectorImpl<int> &AssignedSlots) {
  if (Instrs.size() > DandelionSlots)
    return false;

  SmallVector<uint8_t, DandelionSlots> SlotMasks;
  SlotMasks.reserve(Instrs.size());
  for (MachineInstr *MI : Instrs) {
    uint8_t Mask = getSlotMask(MI->getDesc().getSchedClass(), IID) & 0xFF;
    if (!Mask)
      return false;
    SlotMasks.push_back(Mask);
  }

  return assignSlotMasks(SlotMasks, MustPrecede, AssignedSlots);
}

bool RISCVVLIW::assignSlots(ArrayRef<MachineInstr *> Instrs,
                            const InstrItineraryData *IID,
                            SmallVectorImpl<int> &AssignedSlots) {
  SlotOrderMatrix NoConstraints{};
  return assignSlots(Instrs, IID, NoConstraints, AssignedSlots);
}

void RISCVVLIW::partitionIntoPackets(ArrayRef<MachineInstr *> Instrs,
                                     const InstrItineraryData *IID,
                                     SmallVectorImpl<SlotPacket> &Packets) {
  Packets.clear();

  SmallVector<MachineInstr *, DandelionSlots> Current;
  SmallVector<int, DandelionSlots> AssignedSlots;
  for (MachineInstr *MI : Instrs) {
    SmallVector<MachineInstr *, DandelionSlots> Trial(Current);
    Trial.push_back(MI);
    if (assignSlots(Trial, IID, AssignedSlots)) {
      Current.push_back(MI);
      continue;
    }

    if (Current.empty())
      report_fatal_error("Dandelion VLIW instruction has no legal slot");

    SlotPacket Packet;
    if (!assignSlots(Current, IID, Packet.Slots))
      report_fatal_error("Dandelion VLIW packet has no legal slot assignment");
    Packet.Instrs = Current;
    Packets.push_back(std::move(Packet));

    Current.clear();
    Current.push_back(MI);
    if (!assignSlots(Current, IID, AssignedSlots))
      report_fatal_error("Dandelion VLIW instruction has no legal slot");
  }

  if (!Current.empty()) {
    SlotPacket Packet;
    if (!assignSlots(Current, IID, Packet.Slots))
      report_fatal_error("Dandelion VLIW packet has no legal slot assignment");
    Packet.Instrs = Current;
    Packets.push_back(std::move(Packet));
  }
}
