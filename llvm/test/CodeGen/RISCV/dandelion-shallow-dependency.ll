; RUN: llc -mtriple=riscv32 -mcpu=dandelion -mattr=+m,+f,-d \
; RUN:   -stop-after=riscv-pack-padding -verify-machineinstrs -o - %s \
; RUN:   | FileCheck %s --check-prefix=SHALLOW
; RUN: llc -mtriple=riscv32 -mcpu=dandelion -mattr=+m,+f,-d \
; RUN:   -riscv-disable-shallow-dependency \
; RUN:   -stop-after=riscv-pack-padding -verify-machineinstrs -o - %s \
; RUN:   | FileCheck %s --check-prefix=BASELINE

define i32 @shallow_pair(i32 %a, i32 %b) {
entry:
  %producer = add i32 %a, 1
  %consumer = xor i32 %producer, %b
  ret i32 %consumer
}

; SHALLOW-LABEL: name: shallow_pair
; SHALLOW: BUNDLE 3640313480,
; SHALLOW: ADDI
; SHALLOW-NEXT: renamable $x10 = XOR internal
; BASELINE-LABEL: name: shallow_pair
; BASELINE: BUNDLE 3640313480,
; BASELINE: ADDI
; BASELINE: BUNDLE 3640313480,
; BASELINE: XOR

define i32 @dependent_chain(i32 %a, i32 %b) {
entry:
  %first = add i32 %a, 1
  %second = xor i32 %first, %b
  %third = sub i32 %second, %a
  ret i32 %third
}

; A three-operation chain still contains two Data endpoint pairs, so only its
; first producer/consumer pair can be shallow in one packet.
; SHALLOW-LABEL: name: dependent_chain
; SHALLOW: ADDI
; SHALLOW-NEXT: renamable {{.*}} = XOR internal
; SHALLOW: BUNDLE 3640313480,
; SHALLOW: SUB

