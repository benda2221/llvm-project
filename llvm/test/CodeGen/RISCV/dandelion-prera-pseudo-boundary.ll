; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -riscv-dandelion-prera-scheduler=dandelion \
; RUN:   -debug-only=machine-scheduler -o /dev/null %s 2>&1 \
; RUN:   | FileCheck %s
; REQUIRES: asserts

@object = external global i32

; PseudoMovAddr has an ALU itinerary before register allocation and a two-slot
; footprint matching its later LUI+ADDI expansion. It participates in normal
; packet scheduling instead of forming an opaque boundary.
define ptr @address() {
; CHECK-LABEL: MI Converging Scheduling VLIW
; CHECK-NOT: Dandelion VLIW opaque boundary
; CHECK: Dandelion VLIW candidate opcode={{[0-9]+}} packet-size=0 issue-slots=2 legal=1
  ret ptr @object
}
