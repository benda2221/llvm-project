# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is LLVM 21.1.8 with a custom RISC-V VLIW backend targeting **Dandelion** — an 8-issue, 6-stage in-order VLIW processor. The VLIW-specific code lives in `llvm/lib/Target/RISCV/` alongside the standard RISC-V backend.

## Build Commands

```bash
# Configure (already done; build dir is build-debug/)
cmake -S llvm -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DLLVM_TARGETS_TO_BUILD=RISCV

# Build RISC-V backend only (much faster than full LLVM build)
cmake --build build-debug --target LLVMRISCVCodeGen -j$(nproc)

# Build llc (the primary tool for testing)
cmake --build build-debug --target llc -j$(nproc)

# Full build
cmake --build build-debug -j$(nproc)
```

## Testing the VLIW Packetizer

```bash
# Compile C to RISC-V assembly with VLIW packetizer enabled
build-debug/bin/llc -march=riscv32 -mcpu=dandelion -filetype=asm input.c -o output.s

# Disable packetizer (for comparison)
build-debug/bin/llc -march=riscv32 -mcpu=dandelion -riscv-disable-packetizer=true input.c -o output.s

# Disable padding pass only
build-debug/bin/llc -riscv-disable-packetpadding=true ...

# Run llvm-mca performance analysis
build-debug/bin/llvm-mca -march=riscv32 -mcpu=dandelion input.s

# Run LLVM lit tests for RISC-V
build-debug/bin/llvm-lit llvm/test/CodeGen/RISCV/ -j$(nproc)

# Run a single lit test
build-debug/bin/llvm-lit llvm/test/CodeGen/RISCV/specific-test.ll
```

## VLIW Backend Architecture

### Dandelion Processor (8-slot VLIW)

Pipeline: IF → ID → EX1 → EX2 → EX3 → WB

| Slot | Pipeline | Instruction Classes |
|------|----------|---------------------|
| 0 | FDivFPU | IIC_FDiv, IIC_FPU |
| 1 | ALUFPU | IIC_ALU, IIC_FPU, IIC_FPToInt, IIC_Nop |
| 2 | ALUFPU | IIC_ALU, IIC_FPU, IIC_IntToFP, IIC_Nop |
| 3 | ALUiMD | IIC_ALU, IIC_MUL, IIC_DIV, IIC_Nop |
| 4 | ALUiMD | IIC_ALU, IIC_MUL, IIC_DIV, IIC_Nop |
| 5 | ALALSU | IIC_ALU, IIC_Load, IIC_Store, IIC_Atomic, IIC_Nop |
| 6 | ALALSU | IIC_ALU, IIC_Load, IIC_Store, IIC_Atomic, IIC_Nop |
| 7 | ALUBranch | IIC_ALU, IIC_Branch, IIC_Jump, IIC_CSR, IIC_Nop |

**SLOT7 exclusivity rule:** Once a branch/jump is placed in SLOT7, no more instructions can be added to that packet.

### Key Source Files

**`RISCVPacketizer.cpp`** — Core VLIW bundling pass (`createRISCVPacketizerPass()`).
- `isLegalToPacketizeTogether()`: Rejects RAW dependencies and WAW slot-ordering violations.
- `hasWAWHazardWithSlotConstraint()`: Ensures WAW pairs occupy correctly ordered slots (predecessor must be in a lower slot number than successor).
- `shouldAddToPacket()`: Enforces SLOT7 exclusivity.

**`RISCVPackPadding.cpp`** — Post-packetizer padding pass (`createRISCVPackPaddingPass()`).
- Expands every BUNDLE to exactly 8 instructions by filling empty slots with NOPs.
- SLOT0 NOP: `FEQ.S x0, f0, f0`; SLOT1–7 NOP: `ADDI x0, x0, 0`.
- Greedy left-to-right slot assignment using itinerary `InstrStage` FuncUnit bitmasks.

**`RISCVRelayout.cpp`** — Post-unpack branch relaxation pass (`createRISCVRelayoutPass()`).
- Mirrors `BranchRelaxation.cpp` but runs after bundle unpacking.
- Handles far conditional branches (invert + unconditional jump) and trampolines for cross-section branches.

**`RISCVScheduleVLIW.td`** — Defines 8 `FuncUnit` records (`RISCV_SLOT0`–`RISCV_SLOT7`) and all IIC itinerary classes.

**`RISCVSchedDandelion.td`** — Per-instruction itinerary data with bypass networks:
- `Dandelion_EX2_Bypass`: ALU/branch results forwarded at EX2.
- `Dandelion_EX3_Bypass`: ALU forwarded at EX3.
- `Dandelion_WB_Bypass`: Slow-unit results (MUL, DIV, FPU, Load) forwarded at WB.

### Pass Pipeline Order (in `addPreEmitPass2()`)

```
createRISCVExpandPseudoPass()
createRISCVExpandAtomicPseudoPass()
createUnpackMachineBundles(KCFI)   ← unbundle KCFI only
createRISCVPacketizerPass()        ← VLIW bundling
createRISCVPackPaddingPass()       ← NOP padding to 8-wide
createUnpackMachineBundles()       ← unbundle for emission
createRISCVRelayoutPass()          ← branch relaxation
```

### TableGen DFA Generation

`CMakeLists.txt` generates `RISCVGenDFAPacketizer.inc` via:
```cmake
tablegen(LLVM RISCVGenDFAPacketizer.inc -gen-dfa-packetizer)
```
This encodes legal slot assignments as DFA states derived from the itinerary data. Rebuild the target after any `.td` changes.

## Documentation

Architecture and design docs are in `doc/` (Chinese):
- `doc/arch.md` — Dandelion pipeline and functional unit specification
- `doc/sched_design.md` — IIC definitions, cycle counts, bypass network design
- `doc/RISCV-Itinerary-DFA-Packetizer-实现指南.md` — Implementation guide
