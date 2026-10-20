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

hlt

align 16
.data:
; First string: ABCDabcdIJKLijkl
dq 0x6463626144434241
dq 0x6c6b6a694c4b4a49
; Second string: Here I Am, Y'aLl
dq 0x4120492065726548
dq 0x6c4c612759202C6D
