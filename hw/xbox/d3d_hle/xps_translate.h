/*
 * xps_translate.h — Xbox pixel-shader assembly ("xps.1.1") -> HLSL translator.
 *
 * Xbox titles can build pixel shaders as *text* ps.1.1-class assembly at
 * runtime and hand them to the D3D8 pixel-shader assembler. Plume cannot
 * consume that text directly,
 * so this module reparses the assembly and emits an equivalent HLSL pixel
 * shader (entry point `main`, target ps_6_0) that dxc can compile to
 * DXIL/SPIR-V for a Plume RenderShader.
 *
 * Contract of the emitted HLSL (what the Plume pipeline must bind):
 *   Texture2D   tex0..tex3   : register(t0..t3)   <- D3D8 texture stages 0..3
 *   SamplerState samp0..samp3: register(s0..s3)
 *   cbuffer XPS_Consts       : register(b8)
 *     c[8] at float4 slots 0..7 and per-stage texcoord_scale at 20..23
 *   PSIn { SV_Position, COLOR0 (v0), COLOR1 (v1), TEXCOORD0..3 (t0..t3 coords) }
 *   returns r0 as SV_Target.
 *
 */
#ifndef XRECOMP_XPS_TRANSLATE_H
#define XRECOMP_XPS_TRANSLATE_H

#include <string>

struct XpsTranslateResult {
    bool        ok;        /* true if every opcode was supported */
    std::string hlsl;      /* emitted HLSL (best-effort even when !ok) */
    std::string warnings;  /* '\n'-joined notes: unsupported ops, approximations */
};

/* Translate one xps.1.1 assembly source string to an HLSL pixel shader. */
XpsTranslateResult xrecomp_xps_to_hlsl(const char *src);

#endif /* XRECOMP_XPS_TRANSLATE_H */
