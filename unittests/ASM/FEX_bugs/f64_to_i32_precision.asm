%ifdef CONFIG
{
  "HostFeatures": ["AVX"],
  "Env": { "FEX_HOSTFEATURES": "disablesve,disablefrintts" },
  "RegData": {
    "XMM0": ["0x297e3ce700000000", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"]
  }
}
%endif

; The non-SVE Vector_F64ToI32 path must convert f64 directly to i64 before
; narrowing. Narrowing f64 to f32 first loses precision for this input.
lea rdx, [rel .data]
vcvttpd2dq xmm0, oword [rdx]
hlt

align 16
.data:
dq 0x8d482dd627d581f1, 0x41c4bf1e7380f30a
