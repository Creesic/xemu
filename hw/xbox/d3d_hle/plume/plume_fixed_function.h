/*
 * Shared shader sources for Plume's fixed-function fallbacks.
 *
 * Xbox pitch-linear textures use texel-space coordinates. The draw frontend
 * snapshots four D3DTSS stages in b8 slots 8..17 and reciprocal texture
 * dimensions in slots 20..23.
 */
#ifndef XGPU_PLUME_FIXED_FUNCTION_H
#define XGPU_PLUME_FIXED_FUNCTION_H

#include <string>

namespace xgpu {
namespace plume {

inline uint32_t plumeSelectDrawPixelShader(
    uint32_t activeShader, bool activeShaderValid, bool zPerspective,
    uint32_t fixedFallbackShader, uint32_t fixedFallbackWShader,
    bool syntheticHostFrame = false)
{
    /*
     * Host-decoded RGBA frames are already complete images. They use the
     * frontend's simple textured blit (psHandle == 0), not Xbox fixed-function
     * state left behind by the guest.
     */
    if (syntheticHostFrame)
        return 0;
    if (activeShader && activeShaderValid)
        return activeShader;
    return zPerspective ? fixedFallbackWShader : fixedFallbackShader;
}

inline const char *plumeFixedFallbackPixelShaderHlsl()
{
    return
        "Texture2D<float4> tex0 : register(t0);\n"
        "Texture2D<float4> tex1 : register(t1);\n"
        "Texture2D<float4> tex2 : register(t2);\n"
        "Texture2D<float4> tex3 : register(t3);\n"
        "SamplerState samp0 : register(s4);\n"
        "SamplerState samp1 : register(s5);\n"
        "SamplerState samp2 : register(s6);\n"
        "SamplerState samp3 : register(s7);\n"
        "cbuffer FixedFallbackCB : register(b8) {\n"
        "  float4 fixed_constants[21];\n"
        "  float4 texcoord_scale[4];\n"
        "  uint4 stipple_pattern[8];\n"
        "  uint4 stipple_control;\n"
        "};\n"
        "struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; "
        "float4 spec : COLOR1; float4 uv0 : TEXCOORD0; "
        "float4 uv1 : TEXCOORD1; float4 uv2 : TEXCOORD2; "
        "float4 uv3 : TEXCOORD3; };\n"
        "float4 ff_texture(int stage, PSIn i) {\n"
        "  if (stage == 0) return tex0.Sample(samp0, "
        "i.uv0.xy * texcoord_scale[0].xy);\n"
        "  if (stage == 1) return tex1.Sample(samp1, "
        "i.uv1.xy * texcoord_scale[1].xy);\n"
        "  if (stage == 2) return tex2.Sample(samp2, "
        "i.uv2.xy * texcoord_scale[2].xy);\n"
        "  return tex3.Sample(samp3, "
        "i.uv3.xy * texcoord_scale[3].xy);\n"
        "}\n"
        "float4 ff_arg(int selector, float4 diffuse, float4 current, "
        "float4 texture_color, float4 tfactor, float4 specular) {\n"
        "  int source = selector & 15;\n"
        "  float4 value = diffuse;\n"
        "  if (source == 1) value = current;\n"
        "  else if (source == 2) value = texture_color;\n"
        "  else if (source == 3) value = tfactor;\n"
        "  else if (source == 4) value = specular;\n"
        "  if ((selector & 32) != 0) value = value.aaaa;\n"
        "  if ((selector & 16) != 0) value = 1.0 - value;\n"
        "  return value;\n"
        "}\n"
        "float4 ff_op(int op, float4 a, float4 b, float4 c, float4 diffuse, "
        "float4 texture_color, float4 tfactor, float4 current) {\n"
        "  if (op == 1) return current;\n"
        "  if (op == 2) return a;\n"
        "  if (op == 3) return b;\n"
        "  if (op == 4) return a * b;\n"
        "  if (op == 5) return 2.0 * a * b;\n"
        "  if (op == 6) return 4.0 * a * b;\n"
        "  if (op == 7) return a + b;\n"
        "  if (op == 8) return a + b - 0.5;\n"
        "  if (op == 9) return 2.0 * (a + b - 0.5);\n"
        "  if (op == 10) return a - b;\n"
        "  if (op == 11) return a + b * (1.0 - a);\n"
        "  if (op == 12) return lerp(b, a, diffuse.a);\n"
        "  if (op == 13) return lerp(b, a, texture_color.a);\n"
        "  if (op == 14) return lerp(b, a, tfactor.a);\n"
        "  if (op == 15) return lerp(b, a, current.a);\n"
        "  if (op == 16) return a * b;\n"
        "  if (op == 24) { float d = dot(a.rgb * 2.0 - 1.0, "
        "b.rgb * 2.0 - 1.0); return d.xxxx; }\n"
        "  if (op == 25) return a + b * c;\n"
        "  if (op == 26) return lerp(b, a, c);\n"
        "  return a;\n"
        "}\n"
        "bool ff_alpha_pass(float alpha, float reference, int func) {\n"
        "  if (func == 1) return false;\n"
        "  if (func == 2) return alpha < reference;\n"
        "  if (func == 3) return alpha == reference;\n"
        "  if (func == 4) return alpha <= reference;\n"
        "  if (func == 5) return alpha > reference;\n"
        "  if (func == 6) return alpha != reference;\n"
        "  if (func == 7) return alpha >= reference;\n"
        "  return true;\n"
        "}\n"
        "float4 main(PSIn i) : SV_TARGET {\n"
        "  uint2 stipple_pixel = uint2(i.pos.xy / "
        "max(float(stipple_control.y), 1.0));\n"
        "  uint stipple_row = stipple_pixel.y & 31u;\n"
        "  if (stipple_control.x != 0u && "
        "(stipple_pattern[stipple_row >> 2u][stipple_row & 3u] & "
        "(1u << (stipple_pixel.x & 31u))) == 0u) discard;\n"
        "  float4 texture_color = ff_texture(0, i);\n"
        "  float4 tfactor = fixed_constants[16];\n"
        "  float4 current = i.col;\n"
        "  float4 result;\n"
        "  int writes_texcoord_mask = "
        "(int)(fixed_constants[17].x + 0.5);\n"
        "  bool fixed_state_valid = fixed_constants[17].y != 0.0;\n"
        "  if (!fixed_state_valid) {\n"
        "    float4 default_texcoord0 = float4(0, 0, 0, 1);\n"
        "    float texcoord_signal = dot(abs(i.uv0 - default_texcoord0), "
        "1.0.xxxx) + dot(abs(ddx(i.uv0)) + abs(ddy(i.uv0)), 1.0.xxxx);\n"
        "    bool carries_texcoord0 = (writes_texcoord_mask & 1) != 0 && "
        "texcoord_signal > 1e-7;\n"
        "    if (!carries_texcoord0)\n"
        "      result = saturate(float4(i.col.rgb + i.spec.rgb, i.col.a));\n"
        "    else\n"
        "      result = saturate(float4(texture_color.rgb * i.col.rgb + "
        "i.spec.rgb, texture_color.a * i.col.a));\n"
        "  } else {\n"
        "    [unroll] for (int stage = 0; stage < 4; ++stage) {\n"
        "      float4 color_state = fixed_constants[8 + stage * 2];\n"
        "      float4 alpha_state = fixed_constants[9 + stage * 2];\n"
        "      int color_op = (int)(color_state.x + 0.5);\n"
        "      if (color_op == 1) break;\n"
        "      int color_arg0 = (int)(color_state.y + 0.5);\n"
        "      int color_arg1 = (int)(color_state.z + 0.5);\n"
        "      int color_arg2 = (int)(color_state.w + 0.5);\n"
        "      int alpha_op = (int)(alpha_state.x + 0.5);\n"
        "      int alpha_arg0 = (int)(alpha_state.y + 0.5);\n"
        "      int alpha_arg1 = (int)(alpha_state.z + 0.5);\n"
        "      int alpha_arg2 = (int)(alpha_state.w + 0.5);\n"
        "      texture_color = ff_texture(stage, i);\n"
        "      float4 color_c = ff_arg(color_arg0, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 color_a = ff_arg(color_arg1, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 color_b = ff_arg(color_arg2, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 alpha_c = ff_arg(alpha_arg0, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 alpha_a = ff_arg(alpha_arg1, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 alpha_b = ff_arg(alpha_arg2, i.col, current, "
        "texture_color, tfactor, i.spec);\n"
        "      float4 color = ff_op(color_op, color_a, color_b, color_c, "
        "i.col, texture_color, tfactor, current);\n"
        "      float alpha = current.a;\n"
        "      if (alpha_op != 1)\n"
        "        alpha = ff_op(alpha_op, alpha_a, alpha_b, alpha_c, "
        "i.col, texture_color, tfactor, current).a;\n"
        "      current = float4(color.rgb, alpha);\n"
        "    }\n"
        "    result = saturate(float4(current.rgb + i.spec.rgb, current.a));\n"
        "  }\n"
        "  bool alpha_test_enable = fixed_constants[18].x != 0.0;\n"
        "  int alpha_func = (int)(fixed_constants[18].y + 0.5);\n"
        "  float alpha_ref = fixed_constants[18].z;\n"
        "  if (alpha_test_enable && "
        "!ff_alpha_pass(result.a, alpha_ref, alpha_func)) discard;\n"
        "  return result;\n"
        "}\n";
}

inline const char *plumeFixedFallbackWPixelShaderHlsl()
{
    static const std::string hlsl = [] {
        std::string source = plumeFixedFallbackPixelShaderHlsl();
        const std::string signature =
            "float4 main(PSIn i) : SV_TARGET {\n";
        const std::string replacement =
            "float4 main(PSIn i, out float out_depth : SV_Depth) "
            ": SV_TARGET {\n"
            "  out_depth = saturate(i.pos.w * (1.0 / 16777216.0));\n";
        const size_t position = source.find(signature);
        if (position == std::string::npos)
            return std::string();
        source.replace(position, signature.size(), replacement);
        return source;
    }();
    return hlsl.c_str();
}

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_FIXED_FUNCTION_H */
