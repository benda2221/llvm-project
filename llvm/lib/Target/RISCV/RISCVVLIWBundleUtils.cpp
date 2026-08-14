//===-- RISCVVLIWBundleUtils.cpp - Dandelion bundle helpers --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVVLIWBundleUtils.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

static DebugLoc getBundleDebugLoc(MachineBasicBlock::instr_iterator FirstMI,
                                  MachineBasicBlock::instr_iterator LastMI) {
  for (auto MII = FirstMI; MII != LastMI; ++MII)
    if (MII->getDebugLoc())
      return MII->getDebugLoc();
  return DebugLoc();
}

void RISCVVLIW::finalizeRISCVBundle(MachineBasicBlock &MBB,
                                    MachineBasicBlock::instr_iterator FirstMI,
                                    MachineBasicBlock::instr_iterator LastMI) {
  assert(FirstMI != LastMI && "Cannot finalize an empty Dandelion bundle");

  MIBundleBuilder Bundle(MBB, FirstMI, LastMI);
  MachineFunction &MF = *MBB.getParent();
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  MachineInstrBuilder MIB = BuildMI(MF, getBundleDebugLoc(FirstMI, LastMI),
                                    TII->get(TargetOpcode::BUNDLE));
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

    // Dandelion has no intra-packet result forwarding. Clear any ordering-
    // derived marker left by an earlier generic finalization before collecting
    // every member use as a packet-entry use.
    for (MachineOperand &MO : MII->operands())
      if (MO.isReg() && MO.isInternalRead())
        MO.setIsInternalRead(false);

    for (MachineOperand &MO : MII->all_uses()) {
      Register Reg = MO.getReg();
      if (!Reg)
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
}
