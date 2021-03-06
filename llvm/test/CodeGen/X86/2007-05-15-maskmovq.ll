; RUN: llc < %s -mcpu=yonah

target datalayout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:64:64-v128:128:128-a0:0:64"
target triple = "i686-apple-darwin8"

define void @test(<1 x i64> %c64, <1 x i64> %mask1, i8* %P) {
entry:
	%tmp4 = bitcast <1 x i64> %mask1 to <8 x i8>		; <<8 x i8>> [#uses=1]
	%tmp6 = bitcast <1 x i64> %c64 to <8 x i8>		; <<8 x i8>> [#uses=1]
	tail call void @llvm.x86.mmx.maskmovq( <8 x i8> %tmp6, <8 x i8> %tmp4, i8* %P )
	ret void
}

declare void @llvm.x86.mmx.maskmovq(<8 x i8>, <8 x i8>, i8*)
