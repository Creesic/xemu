/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator
 *
 * The Xbox NV2A GPU uses "register combiners" rather than traditional
 * pixel shaders. Games configure up to 8 general combiner stages plus
 * one final combiner stage. Each general combiner stage performs
 * independent RGB and alpha math on a register file that includes
 * texture samples, interpolated vertex colors, constants, and results
 * from previous stages.
 *
 * Register combiner pipeline overview:
 *
 *   Texture fetch -> [Stage 0] -> [Stage 1] -> ... -> [Stage N-1] -> [Final Combiner] -> output
 *
 * Each general combiner stage computes:
 *   AB = map(A) * map(B)      (per-component multiply)
 *   CD = map(C) * map(D)      (per-component multiply)
 *   output = AB + CD           (or AB dot CD if dot product flag set)
 *   output = scale_bias(output) (optional output mapping)
 *
 * RGB and alpha paths are fully independent per stage - different
 * inputs, different output registers, different flags.
 *
 * The final combiner computes:
 *   result.rgb = A*B + (1-A)*C + D
 *   result.a   = G.a
 * where A,B,C,D,E,F,G each select from the register file.
 * The product E*F is available as a special register (EF_PROD).
 * The sum V1+R0 is also available (V1R0_SUM / SPARE0).
 *
 * This module translates the combiner configuration into portable HLSL.
 *
 * References:
 *   - NV_register_combiners / NV_register_combiners2 GL extensions
 *   - Xbox SDK D3D pixel shader documentation
 *   - xemu NV2A pgraph register combiner implementation
 */

#ifndef XBOXRECOMP_D3D8_COMBINERS_H
#define XBOXRECOMP_D3D8_COMBINERS_H

#include <stdint.h>
#include "d3d8_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Register Combiner Enumerations
 * ================================================================ */

/**
 * Input register selectors.
 *
 * These identify which register in the NV2A register file is read
 * as input to a combiner stage. The register file is shared across
 * all stages; writes from stage N are visible to stage N+1.
 */
typedef enum NV2ACombinerRegister {
    NV2A_REG_ZERO       = 0,   /* Constant zero (reads 0.0) */
    NV2A_REG_C0         = 1,   /* Per-stage constant color 0 */
    NV2A_REG_C1         = 2,   /* Per-stage constant color 1 */
    NV2A_REG_FOG        = 3,   /* Fog factor (interpolated) */
    NV2A_REG_V0         = 4,   /* Primary color (diffuse) */
    NV2A_REG_V1         = 5,   /* Secondary color (specular) */
    /* 6, 7 reserved */
    NV2A_REG_T0         = 8,   /* Texture 0 sample result */
    NV2A_REG_T1         = 9,   /* Texture 1 sample result */
    NV2A_REG_T2         = 10,  /* Texture 2 sample result */
    NV2A_REG_T3         = 11,  /* Texture 3 sample result */
    NV2A_REG_R0         = 12,  /* Temporary register 0 (also SPARE0) */
    NV2A_REG_R1         = 13,  /* Temporary register 1 (also SPARE1) */
    /* Final combiner only: */
    NV2A_REG_V1R0_SUM   = 14,  /* V1+R0 sum (final combiner) */
    NV2A_REG_EF_PROD    = 15,  /* E*F product (final combiner) */
    NV2A_REG_COUNT       = 16,
} NV2ACombinerRegister;

/* Aliases used in documentation and comments */
#define NV2A_REG_SPARE0  NV2A_REG_R0
#define NV2A_REG_SPARE1  NV2A_REG_R1

/**
 * Input mapping modes.
 *
 * Applied to the selected register value before it enters the
 * multiply. These implement common math operations as a pre-step.
 *
 * All inputs are clamped to [0,1] after texture sampling / interpolation,
 * then these mappings produce the final value fed to the multiplier:
 *
 *   UNSIGNED_IDENTITY : x        (passthrough, range [0,1])
 *   UNSIGNED_INVERT   : 1 - x    (complement, range [0,1])
 *   EXPAND_NORMAL     : 2x - 1   (expand to [-1,1])
 *   EXPAND_NEGATE     : 1 - 2x   (expand + negate)
 *   HALFBIAS_NORMAL   : x - 0.5  (shift to [-0.5,0.5])
 *   HALFBIAS_NEGATE   : 0.5 - x  (shift + negate)
 *   SIGNED_IDENTITY   : x        (signed passthrough, allows negative)
 *   SIGNED_NEGATE     : -x       (negate)
 */
typedef enum NV2AInputMapping {
    NV2A_MAP_UNSIGNED_IDENTITY = 0,
    NV2A_MAP_UNSIGNED_INVERT   = 1,
    NV2A_MAP_EXPAND_NORMAL     = 2,
    NV2A_MAP_EXPAND_NEGATE     = 3,
    NV2A_MAP_HALFBIAS_NORMAL   = 4,
    NV2A_MAP_HALFBIAS_NEGATE   = 5,
    NV2A_MAP_SIGNED_IDENTITY   = 6,
    NV2A_MAP_SIGNED_NEGATE     = 7,
    NV2A_MAP_COUNT             = 8,
} NV2AInputMapping;

/**
 * Output scale/bias modes.
 *
 * Applied to the stage output (AB+CD or AB.CD) before writing
 * to the destination register.
 */
typedef enum NV2AOutputMapping {
    NV2A_OUT_IDENTITY           = 0,  /* x */
    NV2A_OUT_BIAS               = 1,  /* x - 0.5 */
    NV2A_OUT_SHIFTLEFT_1        = 2,  /* x * 2 */
    NV2A_OUT_SHIFTLEFT_1_BIAS   = 3,  /* (x - 0.5) * 2 */
    NV2A_OUT_SHIFTLEFT_2        = 4,  /* x * 4 */
    NV2A_OUT_SHIFTRIGHT_1       = 5,  /* x / 2 */
    NV2A_OUT_COUNT              = 6,
} NV2AOutputMapping;

/**
 * Texture addressing modes per stage.
 * Encoded in the pixel shader DWORD token.
 */
typedef enum NV2ATextureMode {
    NV2A_TEXMODE_2D      = 0,  /* Standard 2D texture */
    NV2A_TEXMODE_3D      = 1,  /* 3D / volume texture */
    NV2A_TEXMODE_CUBEMAP = 2,  /* Cube map */
    NV2A_TEXMODE_NONE    = 3,  /* No texture bound */
} NV2ATextureMode;

/* ================================================================
 * Combiner Stage Input / Output Descriptors
 * ================================================================ */

/**
 * A single combiner input selection.
 *
 * Packed in hardware as 8 bits:
 *   [3:0] register   - which register to read (NV2ACombinerRegister)
 *   [4]   alpha_rep   - 0: use full RGBA, 1: replicate alpha to all channels
 *   [7:5] mapping    - input mapping mode (NV2AInputMapping)
 */
typedef struct NV2ACombinerInput {
    NV2ACombinerRegister reg;       /* Source register */
    int                  alpha_rep; /* 1 = replicate alpha to RGB */
    NV2AInputMapping     mapping;   /* Input mapping function */
} NV2ACombinerInput;

/**
 * Output configuration for one channel (RGB or alpha) of a stage.
 *
 * Packed in hardware output word:
 *   [3:0]  ab_dst       - destination register for AB product
 *   [7:4]  cd_dst       - destination register for CD product
 *   [11:8] sum_dst      - destination register for AB+CD sum
 *   [12]   cd_dot       - 1: CD uses dot product instead of multiply
 *   [13]   ab_dot       - 1: AB uses dot product instead of multiply
 *   [14]   mux_sum      - 1: mux instead of sum (R0.a selects AB or CD)
 *   [17:15] output_map  - output scale/bias (NV2AOutputMapping)
 *   [18]   cd_blue_to_alpha - copy mapped CD blue to CD destination alpha
 *   [19]   ab_blue_to_alpha - copy mapped AB blue to AB destination alpha
 *
 * The "dot product" flag means AB = dot3(A, B) instead of A * B
 * component-wise. This is the key to bump mapping on NV2A.
 */
typedef struct NV2ACombinerOutput {
    NV2ACombinerRegister ab_dst;     /* Where to write A*B (or 0=discard) */
    NV2ACombinerRegister cd_dst;     /* Where to write C*D (or 0=discard) */
    NV2ACombinerRegister sum_dst;    /* Where to write AB+CD (or 0=discard) */
    int                  ab_dot;     /* 1 = dot product for AB */
    int                  cd_dot;     /* 1 = dot product for CD */
    int                  mux_sum;    /* 1 = mux(R0.a, AB, CD) instead of AB+CD */
    int                  ab_blue_to_alpha; /* Copy mapped AB.b to ab_dst.a */
    int                  cd_blue_to_alpha; /* Copy mapped CD.b to cd_dst.a */
    NV2AOutputMapping    output_map; /* Scale/bias applied to results */
} NV2ACombinerOutput;

/* ================================================================
 * Full Combiner State
 * ================================================================ */

/** Maximum number of general combiner stages (NV2A hardware limit). */
#define NV2A_MAX_COMBINER_STAGES 8

/** Maximum texture stages. */
#define NV2A_MAX_TEXTURES 4

/**
 * Complete NV2A register combiner configuration.
 *
 * This holds everything needed to generate an equivalent HLSL pixel
 * shader. Games set this up via SetPixelShader (DWORD token) or by
 * individually setting the D3DRS_PS* render states.
 */
typedef struct NV2ACombinerState {
    /* --- General combiner stages --- */
    int num_stages;  /* Active stage count (1-8) */

    struct {
        /* Four inputs per channel: A, B, C, D */
        NV2ACombinerInput  rgb_input[4];
        NV2ACombinerInput  alpha_input[4];
        NV2ACombinerOutput rgb_output;
        NV2ACombinerOutput alpha_output;
    } stages[NV2A_MAX_COMBINER_STAGES];

    /* --- Final combiner --- */
    NV2ACombinerInput final_input[7]; /* A, B, C, D, E, F, G */
    int final_enabled;     /* Final input words are nonzero */
    int final_clamp_sum;   /* V1+R0 clamp to [0,1] */
    int final_inv_v1;      /* complement V1 before V1R0 sum */
    int final_inv_r0;      /* complement R0 before V1R0 sum */

    /* --- Per-stage constant colors --- */
    DWORD c0[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */
    DWORD c1[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */

    /* --- Final combiner constants --- */
    DWORD final_c0; /* Final combiner C0 (same as stage[final].c0) */
    DWORD final_c1; /* Final combiner C1 (same as stage[final].c1) */

    /* --- Texture modes --- */
    NV2ATextureMode tex_mode[NV2A_MAX_TEXTURES];

    /* Raw NV2A texture-shader state used by the LLE PGRAPH bridge. The D3D8
     * token path leaves use_texture_shader clear and retains tex_mode above. */
    int use_texture_shader;
    uint8_t texture_shader_mode[NV2A_MAX_TEXTURES];
    uint8_t texture_dimensions[NV2A_MAX_TEXTURES];
    /* The NV2A texture-shader operation and the bound resource type are
     * independent. PROJECT2D, for example, samples a cube resource through
     * the Xbox 2D-projective-to-cube remap when CUBEMAP_ENABLE is set. */
    uint8_t texture_cube[NV2A_MAX_TEXTURES];
    uint8_t dot_map[NV2A_MAX_TEXTURES];
    uint8_t input_texture[NV2A_MAX_TEXTURES];
    /* Single-channel luminance (LU_IMAGE_Y16) uploads as R16_UNORM, which
     * HLSL samples as (R,0,0,1). Hardware replicates the channel, so flagged
     * stages must be swizzled to (R,R,R,1) after sampling. */
    uint8_t texture_luminance[NV2A_MAX_TEXTURES];

    /* NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE: NV2A writes a W-BUFFER, storing
     * perspective-correct w instead of the screen-linear z/w a GPU depth buffer
     * produces. D3D12 has no such mode, so the pixel shader emits SV_Depth.
     * Requires invalidating the combiner shader when the bit toggles -- MM3
     * flips it ~16x/frame and SET_CONTROL0 is not a combiner register. */
    uint8_t z_perspective;

    /* Bump-environment state for texture-shader modes 6 (BUMPENVMAP) and
     * 7 (BUMPENVMAP_LUM), per stage: 2x2 texcoord perturbation matrix in
     * m00, m01, m10, m11 order plus luminance scale/offset. Baked into the
     * generated HLSL as literals. */
    float bump_env_mat[NV2A_MAX_TEXTURES][4];
    float bump_env_scale[NV2A_MAX_TEXTURES];
    float bump_env_offset[NV2A_MAX_TEXTURES];

    /* --- Flags --- */
    DWORD flags;     /* Dot mapping and other flags from token bits 24-31 */
} NV2ACombinerState;

/* ================================================================
 * PS Constant Buffer (HLSL layout - must be 16-byte aligned)
 *
 * This is uploaded to the GPU every draw call with current values
 * of the combiner constants, fog parameters, etc.
 * ================================================================ */

typedef struct NV2APSConstants {
    float c0[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C0 (RGBA float) */
    float c1[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C1 (RGBA float) */
    float final_c0[4];                      /* Final combiner C0 */
    float final_c1[4];                      /* Final combiner C1 */
    float fog_color[4];                     /* Fog color (from D3DRS_FOGCOLOR) */
    float alpha_ref;                        /* Normalized [0,1] alpha ref */
    UINT  alpha_func;                       /* D3DCMPFUNC enum value */
    UINT  alpha_test_enable;                /* 0 or 1 */
    UINT  fog_enable;                       /* 0 or 1 */
    UINT  fog_mode;                         /* D3DRS_FOGTABLEMODE value */
    float fog_start;                        /* D3DRS_FOGSTART (LINEAR) */
    float fog_end;                          /* D3DRS_FOGEND (LINEAR) */
    float fog_density;                      /* D3DRS_FOGDENSITY (EXP/EXP2) */
} NV2APSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the combiner translator system.
 * Resets the translator state and title-neutral source cache.
 */
HRESULT d3d8_combiners_init(void);

/**
 * Shut down the combiner system.
 */
void d3d8_combiners_shutdown(void);

/**
 * Parse an Xbox pixel shader DWORD token into combiner state.
 *
 * The token encodes the combiner count and texture modes.
 * The actual combiner stage inputs/outputs come from the
 * D3DRS_PS* render states which must already be set.
 *
 * @param token  The DWORD passed to SetPixelShader
 * @param rs     Pointer to the render state array (from d3d8_GetRenderStates)
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state);

/**
 * Build combiner state from individual render states.
 *
 * Called when games set PS render states directly rather than
 * using a pixel shader token.
 *
 * @param rs     Pointer to the render state array
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state);

/**
 * Decode an Xbox D3DPIXELSHADERDEF (60 DWORDs) into shared combiner state.
 */
void d3d8_combiners_parse_definition(
    const DWORD definition[60], NV2ACombinerState *state);

/**
 * Generate HLSL pixel shader source from combiner state.
 *
 * @param state   The combiner configuration
 * @param buf     Output buffer for HLSL source
 * @param bufsize Size of output buffer in bytes
 * @return        Number of characters written (excluding null terminator),
 *                or -1 on error
 */
int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize);

/**
 * Prepare for a draw call using register combiners.
 *
 * This is the main integration point. Call this instead of (or after)
 * d3d8_shaders_prepare_draw() when a combiner pixel shader is active.
 *
 * - Rebuilds combiner state from current render states if dirty
 * - Registers generated HLSL with the Plume shader registry
 * - Snapshots current C0/C1/fog/alpha values for the draw
 *
 * @return TRUE if a combiner shader was bound, FALSE if falling back
 *         to the fixed-function pixel shader.
 */
BOOL d3d8_combiners_prepare_draw(void);

/**
 * Notify the combiner system that a PS-related render state changed.
 * This marks the combiner state as dirty so it will be rebuilt
 * on the next prepare_draw call.
 */
void d3d8_combiners_mark_dirty(void);

/**
 * Notify the combiner system that only draw-time constant state changed.
 * This refreshes the portable constant buffer without rebuilding or looking
 * up an otherwise identical generated pixel shader.
 */
void d3d8_combiners_mark_constants_dirty(void);

/**
 * Record the resource type currently bound to a texture stage. These fields
 * are separate from the texture-shader operation encoded in the pixel-shader
 * definition. Luminance formats replicate their channel across RGB on Xbox.
 */
void d3d8_combiners_set_texture_binding(
    UINT stage, UINT dimensionality, BOOL cube, BOOL luminance);

/**
 * Select Xbox W-buffer depth output for native-HLE combiner draws.
 *
 * D3DZB_USEW is not part of the pixel-shader definition, so it must be
 * carried separately and included in the generated shader variant.
 */
void d3d8_combiners_set_z_perspective(BOOL enabled);

/**
 * Set the current pixel shader token.
 * Pass 0 to disable combiner shaders and revert to fixed-function.
 */
void d3d8_combiners_set_pixel_shader(DWORD token);

/**
 * Bind an Xbox D3DPIXELSHADERDEF (60 DWORDs). This is the native-HLE path:
 * the definition is decoded directly without replaying NV097 methods.
 */
void d3d8_combiners_set_definition(const DWORD definition[60]);

/**
 * Get whether a combiner pixel shader is currently active.
 */
BOOL d3d8_combiners_active(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_COMBINERS_H */
