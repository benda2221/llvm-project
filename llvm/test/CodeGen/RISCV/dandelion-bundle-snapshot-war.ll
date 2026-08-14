; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -mattr=+m,+f,-d,-unaligned-scalar-mem -verify-machineinstrs \
; RUN:   -stop-after=riscv-pack-padding -o - %s \
; RUN:   | FileCheck %s --check-prefix=MIR
; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -mattr=+m,+f,-d,-unaligned-scalar-mem -verify-machineinstrs \
; RUN:   -stop-before=riscv-asm-printer -o - %s \
; RUN:   | FileCheck %s --check-prefix=PREEMIT
; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -mattr=+m,+f,-d,-unaligned-scalar-mem -verify-machineinstrs \
; RUN:   -filetype=asm -o - %s | FileCheck %s --check-prefix=ASM

; Strict alignment expands this store into shifts and byte stores. Register
; allocation reuses x13 for two adjacent live ranges. The second packet has a
; low-slot x13 definition and a high-slot store that must read and kill the
; packet-entry x13 value. Dandelion has no intra-packet forwarding.
define void @store_i64_align1(ptr %p, i64 %value) {
entry:
  store i64 %value, ptr %p, align 1
  ret void
}

; The packet has both an external killed x13 use and a non-dead x13 output.
; The high-slot store must not become an internal read of the low-slot shift.
; MIR-LABEL: name: store_i64_align1
; MIR: BUNDLE {{.*}}implicit-def $x13{{.*}}implicit killed $x13
; MIR-NEXT: $x0 = FEQ_S
; MIR-NEXT: renamable $x13 = SRLI killed renamable $x11, 16
; MIR-NEXT: $x0 = ADDI
; MIR-NEXT: $x0 = ADDI
; MIR-NEXT: $x0 = ADDI
; MIR-NEXT: SB killed renamable $x13, renamable $x10, 5
; MIR-NOT: internal killed renamable $x13

; Relayout and the final pre-emission verifier must also retain the packet.
; PREEMIT-LABEL: name: store_i64_align1
; PREEMIT: BUNDLE {{.*}}implicit-def $x13{{.*}}implicit killed $x13
; PREEMIT-NEXT: $x0 = FEQ_S
; PREEMIT-NEXT: renamable $x13 = SRLI killed renamable $x11, 16
; PREEMIT-NOT: internal killed renamable $x13

; The BUNDLE header is not emitted. Its eight members are emitted once in
; physical slot order, and the later packet still uses the new x13 value.
; ASM-LABEL: store_i64_align1:
; ASM:       srli a2, a1, 24
; ASM-NEXT:  feq.s zero, ft0, ft0
; ASM-NEXT:  srli a3, a1, 16
; ASM-NEXT:  nop
; ASM-NEXT:  nop
; ASM-NEXT:  nop
; ASM-NEXT:  sb a3, 5(a0)
; ASM-NEXT:  sb a5, 6(a0)
; ASM-NEXT:  nop
; ASM:       sb a3, 2(a0)
