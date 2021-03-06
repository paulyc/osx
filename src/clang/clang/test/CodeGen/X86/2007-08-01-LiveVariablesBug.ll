; RUN: llvm-as < %s | llc -march=x86 | not grep movl

define i8 @t(i8 zeroext  %x, i8 zeroext  %y) zeroext  {
	%tmp2 = add i8 %x, 2
	%tmp4 = add i8 %y, -2
	%tmp5 = mul i8 %tmp4, %tmp2
	ret i8 %tmp5
}
