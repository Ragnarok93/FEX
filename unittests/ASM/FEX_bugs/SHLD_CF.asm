%ifdef CONFIG
{
  "RegData": {
    "RAX": "1",
    "R15": "1",
    "R14": "1",
    "R13": "1",
    "R12": "1"
  }
}
%endif

; SHLD/SHRD with 16-bit operands and count 16 must set CF from the last
; bit shifted out of the destination operand.
mov eax, 1
mov edx, 0xbee8
mov rcx, 0
clc
shld ax, dx, 16
setc cl
mov r15, rcx

mov eax, 1
mov edx, 0xbee8
mov rcx, 16
clc
shld ax, dx, cl
setc cl
mov r14, rcx

mov eax, 0x8eeb
mov edx, 1
mov rcx, 0
clc
shrd ax, dx, 16
setc cl
mov r13, rcx

mov eax, 0x8eeb
mov edx, 1
mov rcx, 16
clc
shrd ax, dx, cl
setc cl
mov r12, rcx

hlt
