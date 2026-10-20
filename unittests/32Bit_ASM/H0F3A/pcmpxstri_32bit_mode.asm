%ifdef CONFIG
{
  "RegData": {
    "RAX": "15",
    "RDX": "16",
    "RCX": "5"
  },
  "Mode": "32BIT",
  "HostFeatures": ["SSE4.2"]
}
%endif

; Keep the SSE4.2 string-index instructions covered in 32-bit guest mode.
; Explicit and implicit lengths both find the first matching character ('I')
; at byte index 5 in the second string. In 32-bit mode PCMPxSTRI must return
; the index in ECX while preserving the explicit input lengths in EAX and EDX.

movaps xmm2, [.data]
movaps xmm3, [.data + 32]

mov eax, 15
mov edx, 16
pcmpestri xmm2, xmm3, 0

pcmpistri xmm2, xmm3, 0

; Force a near rel32 branch to exercise cache-time target patching.
jmp near .after_branch
times 160 db 0x90
.after_branch:
hlt

align 16
.data:
; Each source is stored in a 32-byte slot because the test loads the second
; source from .data + 32. Keep a full 16-byte value in each XMM input.
; First string: ABCDabcdIJKLijkl
dq 0x6463626144434241
dq 0x6c6b6a694c4b4a49
dq 0xffffffffffffffff
dq 0xeeeeeeeeeeeeeeee
; Second string: Here I Am, Y'aLl
dq 0x4120492065726548
dq 0x6c4c612759202C6D
dq 0xdddddddddddddddd
dq 0xcccccccccccccccc
