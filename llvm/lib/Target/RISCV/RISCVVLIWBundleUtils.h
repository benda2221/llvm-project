//===-- RISCVVLIWBundleUtils.h - Dandelion bundle helpers ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H
#define LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H

#include "llvm/CodeGen/MachineBasicBlock.h"

namespace llvm {
namespace RISCVVLIW {

/// Finalize a Dandelion instruction packet.
///
/// Every register use in the packet reads the value at packet entry. Register
/// definitions become visible only at packet exit, so member order must not
/// turn a use into an internal bundle read.
void finalizeRISCVBundle(MachineBasicBlock &MBB,
                         MachineBasicBlock::instr_iterator FirstMI,
                         MachineBasicBlock::instr_iterator LastMI);

} // namespace RISCVVLIW
} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_RISCVVLIWBUNDLEUTILS_H
