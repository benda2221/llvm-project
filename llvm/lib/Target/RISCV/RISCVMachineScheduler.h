//===-- RISCVMachineScheduler.h - RISC-V MI schedulers ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVMACHINESCHEDULER_H
#define LLVM_LIB_TARGET_RISCV_RISCVMACHINESCHEDULER_H

namespace llvm {

struct MachineSchedContext;
class ScheduleDAGMILive;

/// Create LLVM's packet-resource-aware pre-RA scheduler for a RISC-V VLIW
/// subtarget. Target-specific DAG mutations are installed by the target
/// machine after this factory returns.
ScheduleDAGMILive *
createRISCVVLIWMachineScheduler(MachineSchedContext *Context);

} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVMACHINESCHEDULER_H
