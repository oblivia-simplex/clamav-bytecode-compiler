; RUN: llc < %s -march=x86-64 | grep {movzbl	%\[abcd\]h,} | count 4
; RUN: llc < %s -march=x86    > %t
; RUN: grep {incb	%ah} %t | count 3
; RUN: grep {movzbl	%ah,} %t | count 3

; Use h registers. On x86-64, codegen doesn't support general allocation
; of h registers yet, due to x86 encoding complications.

define void @bar64(i64 inreg %x, i8* inreg %p) nounwind {
  %t0 = lshr i64 %x, 8
  %t1 = trunc i64 %t0 to i8
  %t2 = add i8 %t1, 1
  store i8 %t2, i8* %p
  ret void
}

define void @bar32(i32 inreg %x, i8* inreg %p) nounwind {
  %t0 = lshr i32 %x, 8
  %t1 = trunc i32 %t0 to i8
  %t2 = add i8 %t1, 1
  store i8 %t2, i8* %p
  ret void
}

define void @bar16(i16 inreg %x, i8* inreg %p) nounwind {
  %t0 = lshr i16 %x, 8
  %t1 = trunc i16 %t0 to i8
  %t2 = add i8 %t1, 1
  store i8 %t2, i8* %p
  ret void
}

define i64 @qux64(i64 inreg %x) nounwind {
  %t0 = lshr i64 %x, 8
  %t1 = and i64 %t0, 255
  ret i64 %t1
}

define i32 @qux32(i32 inreg %x) nounwind {
  %t0 = lshr i32 %x, 8
  %t1 = and i32 %t0, 255
  ret i32 %t1
}

define i16 @qux16(i16 inreg %x) nounwind {
  %t0 = lshr i16 %x, 8
  ret i16 %t0
}
