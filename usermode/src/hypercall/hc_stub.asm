.code
	send_hypercall proc frame
		push rbx
		.pushreg rbx
		.endprolog
		mov rdx, [rcx + 8]
		mov r8,  [rcx + 16]
		mov r9,  [rcx + 24]
		mov r10, [rcx + 32]
		mov r11, [rcx + 40]
		mov rcx, [rcx]
		xor eax, eax
		cpuid
		pop rbx
		ret
	send_hypercall endp
end
