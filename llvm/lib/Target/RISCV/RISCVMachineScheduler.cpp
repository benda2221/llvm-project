//===-- RISCVMachineScheduler.cpp - RISC-V MI schedulers ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVMachineScheduler.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/VLIWMachineScheduler.h"
#include <memory>

using namespace llvm;

ScheduleDAGMILive *
llvm::createRISCVVLIWMachineScheduler(MachineSchedContext *Context) {
  auto *DAG = new VLIWMachineScheduler(
      Context, std::make_unique<ConvergingVLIWScheduler>());

  // createSchedLive() normally installs this mutation. Preserve the same copy
  // constraints when selecting the VLIW-specific ScheduleDAG implementation.
  DAG->addMutation(createCopyConstrainDAGMutation(DAG->TII, DAG->TRI));
  return DAG;
}
