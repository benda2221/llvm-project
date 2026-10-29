; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -riscv-dandelion-prera-scheduler=dandelion \
; RUN:   -debug-only=machine-scheduler -o /dev/null %s 2>&1 \
; RUN:   | FileCheck %s
; REQUIRES: asserts

@object = external global i32

; PseudoMovAddr has no physical Dandelion itinerary slot before register
; allocation because it expands into LUI+ADDI afterwards.  It must be treated
; as an opaque scheduling boundary rather than rejected as an illegal packet.
define ptr @address() {
; CHECK-LABEL: MI Converging Scheduling VLIW
; CHECK: Dandelion VLIW opaque boundary opcode=
  ret ptr @object
}
