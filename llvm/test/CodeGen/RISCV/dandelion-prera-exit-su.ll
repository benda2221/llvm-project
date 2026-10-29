; RUN: llc -mtriple=riscv32 -mcpu=dandelion \
; RUN:   -riscv-dandelion-prera-scheduler=dandelion -filetype=null %s

; Exercise a bidirectional VLIW scheduling region with multiple live-out and
; artificial edges to ExitSU.  ExitSU has no MachineInstr and must not be
; dereferenced by the zero-latency packet cost heuristic.
define i32 @exit_su_cost(i1 %cond) #0 {
entry:
  %unused = icmp eq i8 0, 0
  br i1 %cond, label %then, label %merge

then:
  %value = load i32, ptr null, align 4
  %kind = load i8, ptr null, align 1
  br label %merge

merge:
  %merged.kind = phi i8 [ %kind, %then ], [ 0, %entry ]
  %merged.value = phi i32 [ %value, %then ], [ 0, %entry ]
  %limit = tail call i32 @llvm.smax.i32(i32 %merged.value, i32 0)
  switch i8 %merged.kind, label %dispatch [
    i8 0, label %next
    i8 108, label %next
    i8 1, label %next
  ]

next:
  %unused2 = icmp eq i8 0, 0
  br label %dispatch

dispatch:
  switch i8 0, label %exit [
    i8 0, label %exit
  ]

exit:
  %result = tail call i32 null(ptr nonnull null, i32 %limit) #1
  ret i32 0
}

declare i32 @llvm.smax.i32(i32, i32)

attributes #0 = { nounwind "no-builtins" }
attributes #1 = { nobuiltin nounwind "no-builtins" }
