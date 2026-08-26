; RUN: llc -mtriple=riscv32 -mcpu=dandelion -mattr=+m,+f,+d \
; RUN:   -stop-after=riscv-packetizer -o - %s | FileCheck %s --check-prefix=MIR
; RUN: llc -mtriple=riscv32 -mcpu=dandelion -mattr=+m,+f,+d \
; RUN:   -filetype=asm -o - %s | FileCheck %s --check-prefix=ASM

; Inline assembly is a packet boundary and remains outside BUNDLE. When it
; contains instructions, the author supplies the complete VLIW packet.
define void @inline_asm_packet() {
entry:
  call void asm sideeffect "feq.s zero, ft0, ft0\0A\09nop\0A\09nop\0A\09nop\0A\09nop\0A\09nop\0A\09nop\0A\09fence.i", ""()
  ret void
}

; MIR-LABEL: name: inline_asm_packet
; MIR: body:
; MIR: INLINEASM
; MIR-NEXT: BUNDLE

; ASM-LABEL: inline_asm_packet:
; ASM: #APP
; ASM-NEXT: feq.s zero, ft0, ft0
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: fence.i
; ASM-NEXT: #NO_APP

; Native fences are solo packets and occupy slot 7.
define void @native_fence(ptr %p) {
entry:
  %value = load volatile i32, ptr %p, align 4
  fence seq_cst
  store volatile i32 %value, ptr %p, align 4
  ret void
}

; MIR-LABEL: name: native_fence
; MIR: BUNDLE {{[0-9]+}} {{{$}}
; MIR-NEXT: FENCE
; MIR-NEXT: }

; ASM-LABEL: native_fence:
; ASM: lw
; ASM: feq.s zero, ft0, ft0
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: nop
; ASM-NEXT: fence rw, rw

; Calls and returns are issuing pseudos at this stage. The Dandelion MC layer
; expands a direct call into its AUIPC plus placeholder-packet template, while
; PseudoRET lowers to JALR. They still occupy one logical slot in their owning
; MIR bundle and must be accepted by bundle-aware emission.
declare void @callee(i32)

define void @call_and_return(i32 %value) {
entry:
  call void @callee(i32 %value)
  ret void
}

; MIR-LABEL: name: call_and_return
; MIR: PseudoCALL
; MIR: PseudoRET

; ASM-LABEL: call_and_return:
; ASM: call callee
; ASM: ret
