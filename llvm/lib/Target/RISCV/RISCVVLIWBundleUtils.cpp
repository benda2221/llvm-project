//===-- RISCVVLIWBundleUtils.cpp - Dandelion bundle helpers --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVVLIWBundleUtils.h"
#include "RISCVVLIWSlotUtils.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

static constexpr uint32_t DandelionSlotMapTag = 0xD;
static constexpr unsigned DandelionSlotMapCountShift = 24;
static constexpr unsigned DandelionSlotMapTagShift = 28;

uint32_t RISCVVLIW::encodeDandelionSlotMap(ArrayRef<int> AssignedSlots) {
  assert(!AssignedSlots.empty() &&
         AssignedSlots.size() <= DandelionSlots &&
         "invalid Dandelion slot-map size");

  uint32_t Encoding = DandelionSlotMapTag << DandelionSlotMapTagShift;
  Encoding |= static_cast<uint32_t>(AssignedSlots.size())
              << DandelionSlotMapCountShift;
  uint8_t Occupied = 0;
  for (unsigned I = 0, E = AssignedSlots.size(); I != E; ++I) {
    assert(AssignedSlots[I] >= 0 && AssignedSlots[I] < (int)DandelionSlots &&
           "invalid Dandelion slot");
    uint8_t SlotBit = static_cast<uint8_t>(1u << AssignedSlots[I]);
    assert(!(Occupied & SlotBit) && "duplicate Dandelion slot");
    Occupied |= SlotBit;
    Encoding |= static_cast<uint32_t>(AssignedSlots[I]) << (I * 3);
  }
  return Encoding;
}

bool RISCVVLIW::decodeDandelionSlotMap(
    const MachineInstr &Bundle, unsigned NumMembers,
    SmallVectorImpl<int> &AssignedSlots) {
  AssignedSlots.clear();
  if (!Bundle.isBundle() || Bundle.getNumOperands() == 0 ||
      !Bundle.getOperand(0).isImm())
    return false;

  uint32_t Encoding = static_cast<uint32_t>(Bundle.getOperand(0).getImm());
  if ((Encoding >> DandelionSlotMapTagShift) != DandelionSlotMapTag)
    return false;
  unsigned Count =
      (Encoding >> DandelionSlotMapCountShift) & 0xF;
  if (Count == 0 || Count > DandelionSlots || Count != NumMembers)
    return false;

  uint8_t Occupied = 0;
  for (unsigned I = 0; I != Count; ++I) {
    int Slot = (Encoding >> (I * 3)) & 0x7;
    uint8_t SlotBit = static_cast<uint8_t>(1u << Slot);
    if (Occupied & SlotBit)
      return false;
    Occupied |= SlotBit;
    AssignedSlots.push_back(Slot);
  }
  return true;
}

bool RISCVVLIW::verifyDandelionSlotMap(
    const MachineInstr &Bundle, ArrayRef<MachineInstr *> Members,
    const InstrItineraryData *IID) {
  SmallVector<int, DandelionSlots> Slots;
  if (!decodeDandelionSlotMap(Bundle, Members.size(), Slots))
    return false;
  for (unsigned I = 0, E = Members.size(); I != E; ++I) {
    uint8_t Mask =
        getSlotMask(Members[I]->getDesc().getSchedClass(), IID) & 0xFF;
    if (!(Mask & (1u << Slots[I])))
      return false;
  }
  return true;
}

static DebugLoc getBundleDebugLoc(MachineBasicBlock::instr_iterator FirstMI,
                                  MachineBasicBlock::instr_iterator LastMI) {
  for (auto MII = FirstMI; MII != LastMI; ++MII)
    if (MII->getDebugLoc())
      return MII->getDebugLoc();
  return DebugLoc();
}

MachineInstr &RISCVVLIW::finalizeRISCVBundle(
    MachineBasicBlock &MBB, MachineBasicBlock::instr_iterator FirstMI,
    MachineBasicBlock::instr_iterator LastMI, ArrayRef<int> AssignedSlots) {
  assert(FirstMI != LastMI && "Cannot finalize an empty Dandelion bundle");

  MIBundleBuilder Bundle(MBB, FirstMI, LastMI);
  MachineFunction &MF = *MBB.getParent();
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  MachineInstrBuilder MIB = BuildMI(MF, getBundleDebugLoc(FirstMI, LastMI),
                                    TII->get(TargetOpcode::BUNDLE));
  MIB.addImm(encodeDandelionSlotMap(AssignedSlots));
  Bundle.prepend(MIB);

  SmallSetVector<Register, 32> PacketDefs;
  SmallSet<Register, 8> DeadDefs;
  SmallSetVector<Register, 16> PacketUses;
  SmallSet<Register, 8> KilledUses;
  SmallSet<Register, 8> UndefUses;
  SmallSet<Register, 8> DefinedUses;

  for (auto MII = FirstMI; MII != LastMI; ++MII) {
    if (MII->isDebugInstr())
      continue;

    for (MachineOperand &MO : MII->all_uses()) {
      Register Reg = MO.getReg();
      if (!Reg)
        continue;

      if (MO.isInternalRead())
        continue;

      PacketUses.insert(Reg);
      if (MO.isKill())
        KilledUses.insert(Reg);
      if (!MO.isUndef()) {
        DefinedUses.insert(Reg);
        UndefUses.erase(Reg);
      } else if (!DefinedUses.contains(Reg)) {
        UndefUses.insert(Reg);
      }
    }

    for (MachineOperand &MO : MII->all_defs()) {
      Register Reg = MO.getReg();
      if (!Reg)
        continue;

      PacketDefs.insert(Reg);
      // Members are in increasing slot order. For WAW, the highest slot wins,
      // so the last definition determines whether the packet output is dead.
      if (MO.isDead())
        DeadDefs.insert(Reg);
      else
        DeadDefs.erase(Reg);

      if (!MO.isDead() && Reg.isPhysical()) {
        for (MCPhysReg SubReg : TRI->subregs(Reg)) {
          PacketDefs.insert(SubReg);
          DeadDefs.erase(SubReg);
        }
      }
    }

    if (MII->getFlag(MachineInstr::FrameSetup))
      MIB.setMIFlag(MachineInstr::FrameSetup);
    if (MII->getFlag(MachineInstr::FrameDestroy))
      MIB.setMIFlag(MachineInstr::FrameDestroy);
  }

  for (Register Reg : PacketDefs)
    MIB.addReg(Reg, getDefRegState(true) |
                        getDeadRegState(DeadDefs.contains(Reg)) |
                        getImplRegState(true));

  for (Register Reg : PacketUses)
    MIB.addReg(Reg, getKillRegState(KilledUses.contains(Reg)) |
                        getUndefRegState(UndefUses.contains(Reg)) |
                        getImplRegState(true));

  return *MIB;
}
