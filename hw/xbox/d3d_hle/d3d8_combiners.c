/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders consumed by the Plume RHI. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test and fog
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of Plume shader handles. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. Structural
 *    state and draw-time constants are versioned independently so repeated
 *    draws reuse the current shader and converted constants.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include "d3d8_texture_state.h"
#include "plume/plume_host.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;
static BOOL g_definition_active = FALSE;
static DWORD g_definition[60];

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;
static uint8_t g_texture_dimensions[NV2A_MAX_TEXTURES];
static uint8_t g_texture_cube[NV2A_MAX_TEXTURES];
static uint8_t g_texture_luminance[NV2A_MAX_TEXTURES];
static uint8_t g_z_perspective;
/* Authored Xbox D3D stage order: m00, m01, m10, m11, scale, offset. */
static float g_bump_env[NV2A_MAX_TEXTURES][6];
static uint8_t g_color_key_mode[NV2A_MAX_TEXTURES];
static uint32_t g_color_key[NV2A_MAX_TEXTURES];
static uint32_t g_color_key_mask[NV2A_MAX_TEXTURES];

/** Dirty flags for generated-shader state and draw-time constants. */
static BOOL g_dirty = TRUE;
static BOOL g_constants_dirty = TRUE;
static uint32_t g_current_shader_handle = 0;
static NV2APSConstants g_constants;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    uint32_t            shader_handle;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, sizeof(NV2ACombinerState));
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, sizeof(NV2ACombinerState)) == 0;
}

/* C0/C1 are read through the per-draw constant buffer and never affect the
 * generated HLSL.  Canonicalize them out of shader identity so animating a
 * color does not generate, hash, and cache duplicate shader source. */
static NV2ACombinerState combiner_shader_state(
    const NV2ACombinerState *state)
{
    NV2ACombinerState shader_state = *state;
    memset(shader_state.c0, 0, sizeof(shader_state.c0));
    memset(shader_state.c1, 0, sizeof(shader_state.c1));
    shader_state.final_c0 = 0;
    shader_state.final_c1 = 0;
    return shader_state;
}

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is packed as 8 bits in the render state DWORDs:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel replicate
 *   [7:5] input mapping mode (NV2AInputMapping value)
 * ================================================================ */

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/**
 * Parse a 32-bit input register DWORD containing 4 packed inputs.
 * Layout: [31:24]=A [23:16]=B [15:8]=C [7:0]=D
 */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/**
 * Parse a 32-bit output configuration DWORD for one channel.
 *
 * Output DWORD layout:
 *   [3:0]   CD destination register
 *   [7:4]   AB destination register
 *   [11:8]  SUM destination register
 *   [12]    CD dot product flag
 *   [13]    AB dot product flag
 *   [14]    mux_sum flag (mux instead of sum)
 *   [17:15] output mapping (scale/bias)
 *   [18]    copy mapped CD blue to CD destination alpha (RGB output only)
 *   [19]    copy mapped AB blue to AB destination alpha (RGB output only)
 */
static void parse_output(DWORD dword, NV2ACombinerOutput *output)
{
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->cd_blue_to_alpha = (dword >> 18) & 1;
    output->ab_blue_to_alpha = (dword >> 19) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
}

static NV2ATextureMode definition_texture_mode(DWORD raw)
{
    switch (raw & 0x1Fu) {
    case 0: case 0x11: return NV2A_TEXMODE_NONE;
    case 2: return NV2A_TEXMODE_3D;
    case 3: return NV2A_TEXMODE_CUBEMAP;
    default: return NV2A_TEXMODE_2D;
    }
}

static uint32_t effective_texture_shader_mode(
    const NV2ACombinerState *state, int stage)
{
    uint32_t mode = state->texture_shader_mode[stage];

    /*
     * Xbox D3D adjusts projective texture modes to the resource type bound at
     * the stage. In particular, a cube resource behind PROJECT2D/PROJECT3D is
     * programmed as CUBEMAP, so the shader consumes the authored xyz
     * direction directly. Retaining the definition's projective divide here
     * samples a different cube direction than the native driver.
     */
    if (state->texture_cube[stage]) {
        if (mode == 1u || mode == 2u || mode == 3u)
            return 3u;
        if (mode == 0x0Du || mode == 0x0Eu)
            return 0x0Eu;
    }
    return mode;
}

static int texture_binding_is_3d(
    const NV2ACombinerState *state, int stage)
{
    /*
     * The texture-shader opcode and the resource dimensionality are separate
     * NV2A state. DOT_STR_3D (0x0D), for example, describes how coordinates
     * are generated; the bound PixelContainer decides whether the resulting
     * three coordinates address a volume or a 2D image. The D3D HLE bridge
     * records that live binding in texture_dimensions. Consulting tex_mode
     * here instead leaves 0x0D at its definition-time 2D default and binds a
     * Texture3D SRV to HLSL declared as Texture2D, which samples zero on
     * D3D12.
     */
    return state->texture_dimensions[stage] == 3u;
}

static void parse_definition_output(
    DWORD dword, NV2ACombinerOutput *output)
{
    DWORD mapping = (dword >> 15) & 7u;
    parse_output(dword, output);
    if (mapping == 6u)
        output->output_map = NV2A_OUT_SHIFTRIGHT_1;
    else if (mapping > 4u)
        output->output_map = NV2A_OUT_IDENTITY;
}

void d3d8_combiners_parse_definition(
    const DWORD definition[60], NV2ACombinerState *state)
{
    DWORD control = definition[53];
    int unique_c0 = (control & 0x1000u) != 0;
    int unique_c1 = (control & 0x10000u) != 0;
    int i;
    memset(state, 0, sizeof(*state));
    state->num_stages = (int)(control & 0xFu);
    if (state->num_stages < 1)
        state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->flags = control >> 8;
    state->use_texture_shader = 1;
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; ++i) {
        parse_four_inputs(
            definition[34 + i], state->stages[i].rgb_input);
        parse_four_inputs(
            definition[i], state->stages[i].alpha_input);
        parse_definition_output(
            definition[45 + i], &state->stages[i].rgb_output);
        parse_definition_output(
            definition[26 + i], &state->stages[i].alpha_output);
        state->c0[i] = definition[10 + (unique_c0 ? i : 0)];
        state->c1[i] = definition[18 + (unique_c1 ? i : 0)];
    }
    parse_four_inputs(definition[8], state->final_input);
    parse_combiner_input(
        definition[9] >> 24, &state->final_input[4]);
    parse_combiner_input(
        definition[9] >> 16, &state->final_input[5]);
    parse_combiner_input(
        definition[9] >> 8, &state->final_input[6]);
    state->final_enabled = definition[8] != 0 || definition[9] != 0;
    state->final_clamp_sum = (definition[9] & 0x80u) != 0;
    state->final_inv_v1 = (definition[9] & 0x40u) != 0;
    state->final_inv_r0 = (definition[9] & 0x20u) != 0;
    state->final_c0 = definition[43];
    state->final_c1 = definition[44];
    for (i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        DWORD raw = (definition[54] >> (i * 5)) & 0x1Fu;
        state->texture_shader_mode[i] = (uint8_t)raw;
        state->tex_mode[i] = definition_texture_mode(raw);
        state->texture_dimensions[i] =
            state->tex_mode[i] == NV2A_TEXMODE_3D ? 3u : 2u;
        state->dot_map[i] = i == 0 ? 0u :
            (uint8_t)((definition[55] >> ((i - 1) * 4)) & 0xFu);
        state->input_texture[i] = i < 2 ? (i == 0 ? 0xFFu : 0u) :
            (uint8_t)((definition[56] >> (8 + i * 4)) & 0xFu);
    }
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    memset(state, 0, sizeof(*state));

    /* Bits [3:0]: number of active combiner stages (1-8) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;

    /* Bits [8:23]: texture mode per stage (4 bits each) */
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);

    /* Bits [24:31]: dot mapping and other flags */
    state->flags = (token >> 24) & 0xFF;

    /* Parse per-stage inputs and outputs from render states */
    d3d8_combiners_from_render_states(rs, state);

    /* Preserve the token-derived fields (from_render_states may overwrite) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);
    state->flags = (token >> 24) & 0xFF;
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    int i;

    /*
     * If called standalone (not from parse_token), read combiner count
     * from D3DRS_PSCOMBINERCOUNT render state.
     *
     * D3DRS_PSCOMBINERCOUNT layout:
     *   [3:0] number of stages
     *   [8]   unique C0 per stage (1) vs shared (0)
     *   [9]   unique C1 per stage (1) vs shared (0)
     *   [16]  mux_MSB for final combiner (not commonly used)
     */
    if (state->num_stages == 0) {
        state->num_stages = rs[D3DRS_PSCOMBINERCOUNT] & 0xF;
        if (state->num_stages < 1) state->num_stages = 1;
        if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
            state->num_stages = NV2A_MAX_COMBINER_STAGES;
    }

    /* Parse stage inputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_four_inputs(rs[D3DRS_PSRGBINPUTS0 + i],
                          state->stages[i].rgb_input);
        parse_four_inputs(rs[D3DRS_PSALPHAINPUTS0 + i],
                          state->stages[i].alpha_input);
    }

    /* Parse stage outputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_output(rs[D3DRS_PSRGBOUTPUTS0 + i],
                     &state->stages[i].rgb_output);
        parse_output(rs[D3DRS_PSALPHAOUTPUTS0 + i],
                     &state->stages[i].alpha_output);
    }

    /*
     * Parse final combiner inputs.
     *
     * D3DRS_PSFINALCOMBINERINPUTSABCD packs inputs A,B,C,D as 8 bits each:
     *   [31:24]=A  [23:16]=B  [15:8]=C  [7:0]=D
     *
     * D3DRS_PSFINALCOMBINERINPUTSEFG packs E,F,G:
     *   [31:24]=E  [23:16]=F  [15:8]=G  [7:0]=settings
     */
    {
        DWORD abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
        DWORD efg  = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];

        state->final_enabled = abcd != 0 || efg != 0;
        parse_combiner_input((abcd >> 24) & 0xFF, &state->final_input[0]); /* A */
        parse_combiner_input((abcd >> 16) & 0xFF, &state->final_input[1]); /* B */
        parse_combiner_input((abcd >>  8) & 0xFF, &state->final_input[2]); /* C */
        parse_combiner_input((abcd >>  0) & 0xFF, &state->final_input[3]); /* D */
        parse_combiner_input((efg  >> 24) & 0xFF, &state->final_input[4]); /* E */
        parse_combiner_input((efg  >> 16) & 0xFF, &state->final_input[5]); /* F */
        parse_combiner_input((efg  >>  8) & 0xFF, &state->final_input[6]); /* G */
        {
            DWORD fc_flags = efg & 0xFFu;
            state->final_clamp_sum = (fc_flags & 0x80u) != 0;
            state->final_inv_v1 = (fc_flags & 0x40u) != 0;
            state->final_inv_r0 = (fc_flags & 0x20u) != 0;
        }
    }

    /* Per-stage constant colors */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }

    /* Final combiner uses the constants from the last active stage, or
     * can use its own - for now, store them separately. Games typically
     * share them with the last stage. */
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];

    /* Read texture modes from render state if not already set by token */
    if (state->tex_mode[0] == 0 && state->tex_mode[1] == 0 &&
        state->tex_mode[2] == 0 && state->tex_mode[3] == 0) {
        DWORD tm = rs[D3DRS_PSTEXTUREMODES];
        state->tex_mode[0] = (NV2ATextureMode)((tm >>  0) & 0xF);
        state->tex_mode[1] = (NV2ATextureMode)((tm >>  4) & 0xF);
        state->tex_mode[2] = (NV2ATextureMode)((tm >>  8) & 0xF);
        state->tex_mode[3] = (NV2ATextureMode)((tm >> 12) & 0xF);
    }
}

/* ================================================================
 * HLSL Code Generation
 *
 * Strategy: build the shader string via snprintf into a large buffer.
 * Each section appends to a running offset. This is not the prettiest
 * approach but it's straightforward, debuggable, and has zero
 * external dependencies.
 *
 * Generated shader structure:
 *   1. Texture sampler declarations
 *   2. Constant buffer (matches NV2APSConstants layout)
 *   3. Input struct (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3)
 *   4. Input mapping helper function
 *   5. Output mapping helper function
 *   6. main():
 *      a. Initialize register file from inputs
 *      b. Execute each general combiner stage
 *      c. Execute final combiner
 *      d. Apply fog
 *      e. Apply alpha test
 *      f. Return result
 * ================================================================ */

/**
 * Emit HLSL to append a string to the output buffer.
 * Returns new offset, or -1 if buffer overflow.
 */
#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/**
 * Get the HLSL variable name for a register in the NV2A register file.
 *
 * The register file is represented as local float4 variables in the
 * generated shader. This returns the name used in the HLSL code.
 */
static const char *reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_ZERO:     return "r_zero";
    case NV2A_REG_C0:       return "r_c0";
    case NV2A_REG_C1:       return "r_c1";
    case NV2A_REG_FOG:      return "r_fog";
    case NV2A_REG_V0:       return "r_v0";
    case NV2A_REG_V1:       return "r_v1";
    case NV2A_REG_T0:       return "r_t0";
    case NV2A_REG_T1:       return "r_t1";
    case NV2A_REG_T2:       return "r_t2";
    case NV2A_REG_T3:       return "r_t3";
    case NV2A_REG_R0:       return "r_r0";
    case NV2A_REG_R1:       return "r_r1";
    case NV2A_REG_EF_PROD:  return "r_ef";
    case NV2A_REG_V1R0_SUM: return "r_v1r0sum";
    default:                return "r_zero";
    }
}

static const char *texture_input_reg_name(
    const NV2ACombinerState *state, int stage)
{
    static const char *const names[NV2A_MAX_TEXTURES] = {
        "r_t0", "r_t1", "r_t2", "r_t3"
    };
    const uint8_t input = state->input_texture[stage];

    /* Dependent texture operations may only read an earlier stage. Reserved
     * PSInputTexture nibbles occur in retail shaders; make them a defined zero
     * input instead of emitting an undeclared r_t15 host variable. */
    return input < stage ? names[input] : "r_zero";
}

/**
 * Emit HLSL expression for reading a combiner input.
 *
 * An input consists of:
 *   1. Register selection (which variable to read)
 *   2. Channel selection (full RGBA or alpha-replicated)
 *   3. Mapping function (how to transform the value)
 *
 * For alpha-replicate: .aaaa swizzle
 * For normal RGB read in RGB path: .rgb (or .rgba for alpha path)
 *
 * The mapping function is applied inline as an arithmetic expression.
 *
 * @param suffix  ".rgb" for RGB path, ".a" for alpha path (determines swizzle)
 */
static void emit_mapped_input(char *buf, int bufsize, int *off,
                               const NV2ACombinerInput *input,
                               const char *suffix, int stage_idx)
{
    const char *rn;
    char swizzle[8];
    char base_expr[128];
    int n;

    rn = reg_name(input->reg);

    /* General stages use their stage-indexed C0/C1 constants. The final
     * combiner has dedicated C0/C1 registers (PSFinalCombinerConstant0/1). */
    if (input->reg == NV2A_REG_C0) {
        if (stage_idx < 0)
            snprintf(base_expr, sizeof(base_expr), "fc0");
        else
            snprintf(base_expr, sizeof(base_expr), "c0[%d]", stage_idx);
    } else if (input->reg == NV2A_REG_C1) {
        if (stage_idx < 0)
            snprintf(base_expr, sizeof(base_expr), "fc1");
        else
            snprintf(base_expr, sizeof(base_expr), "c1[%d]", stage_idx);
    } else {
        snprintf(base_expr, sizeof(base_expr), "%s", rn);
    }

    /* Determine swizzle from the channel-select bit (ICW bit 0x10) and the
     * target channel. Hardware: the RGB portion reads the register's RGB
     * when clear and replicates alpha when set; the ALPHA portion reads the
     * register's BLUE channel when clear and alpha when set (xemu psh.c
     * PS_CHANNEL_BLUE/PS_CHANNEL_ALPHA). MM3's DXT1 font atlases carry
     * glyph coverage in blue and depend on the blue-channel alpha read. */
    if (input->alpha_rep) {
        if (strcmp(suffix, ".a") == 0)
            snprintf(swizzle, sizeof(swizzle), ".a");
        else
            snprintf(swizzle, sizeof(swizzle), ".aaa");
    } else if (strcmp(suffix, ".a") == 0) {
        snprintf(swizzle, sizeof(swizzle), ".b");
    } else {
        snprintf(swizzle, sizeof(swizzle), "%s", suffix);
    }

    /* Build the full variable reference */
    char var_ref[160];
    snprintf(var_ref, sizeof(var_ref), "%s%s", base_expr, swizzle);

    /* Apply input mapping. The unsigned mappings clamp the operand to
     * [0,1] BEFORE the arithmetic (NV_register_combiners); registers can
     * legitimately hold [-1,1], so max() alone is not equivalent —
     * unclamped 1-x reaches 2.0 and inflates downstream products. */
    switch (input->mapping) {
    case NV2A_MAP_UNSIGNED_IDENTITY:
        /* clamp(x, 0, 1) */
        n = snprintf(buf + *off, bufsize - *off, "saturate(%s)", var_ref);
        break;
    case NV2A_MAP_UNSIGNED_INVERT:
        /* 1 - clamp(x, 0, 1) */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - saturate(%s))", var_ref);
        break;
    case NV2A_MAP_EXPAND_NORMAL:
        /* 2*clamp(x, 0, 1) - 1 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(2.0 * saturate(%s) - 1.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NEGATE:
        /* 1 - 2*clamp(x, 0, 1) */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - 2.0 * saturate(%s))", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NORMAL:
        /* clamp(x, 0, 1) - 0.5 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(saturate(%s) - 0.5)", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NEGATE:
        /* 0.5 - clamp(x, 0, 1) */
        n = snprintf(buf + *off, bufsize - *off,
                     "(0.5 - saturate(%s))", var_ref);
        break;
    case NV2A_MAP_SIGNED_IDENTITY:
        /* x (allow negative values) */
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    case NV2A_MAP_SIGNED_NEGATE:
        /* -x */
        n = snprintf(buf + *off, bufsize - *off, "(-%s)", var_ref);
        break;
    default:
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    }
    if (n > 0) *off += n;
}

/**
 * Emit HLSL expression for the output mapping (scale/bias).
 */
static const char *output_map_prefix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return "(";
    case NV2A_OUT_SHIFTLEFT_1:      return "(";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return "((";
    case NV2A_OUT_SHIFTLEFT_2:      return "(";
    case NV2A_OUT_SHIFTRIGHT_1:     return "(";
    default:                        return "";
    }
}

static const char *output_map_suffix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return " - 0.5)";
    case NV2A_OUT_SHIFTLEFT_1:      return " * 2.0)";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return " - 0.5) * 2.0)";
    case NV2A_OUT_SHIFTLEFT_2:      return " * 4.0)";
    case NV2A_OUT_SHIFTRIGHT_1:     return " * 0.5)";
    default:                        return "";
    }
}

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    /* PSDOTMAPPING occupies a four-bit field. Values 0..7 are defined by
     * NV2A; 8..15 are reserved but can remain in inactive retail state.
     * Keep all sixteen indices valid so malformed or stale state can never
     * turn an HLSL format argument into an out-of-bounds host pointer. */
    static const char *dotmap_func[16] = {
        "dotmap_zero_to_one",
        "dotmap_minus1_to_1_d3d",
        "dotmap_minus1_to_1_gl",
        "dotmap_minus1_to_1",
        "dotmap_hilo_1",
        "dotmap_hilo_hemisphere_d3d",
        "dotmap_hilo_hemisphere_gl",
        "dotmap_hilo_hemisphere",
        "dotmap_invalid", "dotmap_invalid", "dotmap_invalid",
        "dotmap_invalid", "dotmap_invalid", "dotmap_invalid",
        "dotmap_invalid", "dotmap_invalid",
    };
    int off = 0;
    int i;

    /* ---- Texture samplers ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        uint32_t shader_mode =
            effective_texture_shader_mode(state, i);
        int sampled = state->use_texture_shader
            ? (shader_mode == 1 ||
               shader_mode == 2 ||
               shader_mode == 3 ||
               shader_mode == 6 ||
               shader_mode == 7 ||
               shader_mode == 0x09 ||
               shader_mode == 0x0B ||
               shader_mode == 0x0C ||
               shader_mode == 0x0D ||
               shader_mode == 0x0E ||
               shader_mode == 0x0F ||
               shader_mode == 0x10)
            : state->tex_mode[i] != NV2A_TEXMODE_NONE;
        if (sampled) {
            if (state->texture_cube[i]) {
                EMIT("TextureCube  tex%d : register(t%d);\n", i, i);
            } else if (texture_binding_is_3d(state, i)) {
                EMIT("Texture3D    tex%d : register(t%d);\n", i, i);
            } else {
                EMIT("Texture2D    tex%d : register(t%d);\n", i, i);
            }
            EMIT("SamplerState samp%d : register(s%d);\n", i, i + 4);
        }
    }
    EMIT("\n");

    /* ---- Constant buffer ---- */
    EMIT("cbuffer CombinerCB : register(b8) {\n");
    EMIT("    float4 c0[8];\n");    /* Per-stage C0 */
    EMIT("    float4 c1[8];\n");    /* Per-stage C1 */
    EMIT("    float4 fc0;\n");      /* Final combiner C0 */
    EMIT("    float4 fc1;\n");      /* Final combiner C1 */
    EMIT("    float4 fog_color;\n");
    EMIT("    float  alpha_ref;\n");
    EMIT("    uint   alpha_func;\n");
    EMIT("    uint   alpha_test_enable;\n");
    EMIT("    uint   fog_enable;\n");
    EMIT("    uint   fog_mode;\n");
    EMIT("    float  fog_start;\n");
    EMIT("    float  fog_end;\n");
    EMIT("    float  fog_density;\n");
    EMIT("    float4 texcoord_scale[4];\n");
    EMIT("};\n\n");

    /* ---- Input structure ---- */
    EMIT("struct PS_IN {\n");
    EMIT("    float4 pos     : SV_POSITION;\n");
    EMIT("    float4 color0  : COLOR0;\n");
    EMIT("    float4 color1  : COLOR1;\n");
    EMIT("    float%d tc0     : TEXCOORD0;\n", state->use_texture_shader ? 4 : 2);
    EMIT("    float%d tc1     : TEXCOORD1;\n", state->use_texture_shader ? 4 : 2);
    EMIT("    float%d tc2     : TEXCOORD2;\n", state->use_texture_shader ? 4 : 2);
    EMIT("    float%d tc3     : TEXCOORD3;\n", state->use_texture_shader ? 4 : 2);
    EMIT("};\n\n");

    if (state->use_texture_shader) {
        EMIT("float sign1(float x) { x *= 255.0; return (x-128.0)/127.0; }\n");
        EMIT("float sign2(float x) { x *= 255.0; return x >= 128.0 ? (x-255.5)/127.5 : (x+0.5)/127.5; }\n");
        EMIT("float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x-256.0)/127.0 : x/127.0; }\n");
        EMIT("float3 dotmap_zero_to_one(float4 c) { return c.rgb; }\n");
        EMIT("float3 dotmap_minus1_to_1_d3d(float4 c) { return float3(sign1(c.r),sign1(c.g),sign1(c.b)); }\n");
        EMIT("float3 dotmap_minus1_to_1_gl(float4 c) { return float3(sign2(c.r),sign2(c.g),sign2(c.b)); }\n");
        EMIT("float3 dotmap_minus1_to_1(float4 c) { return float3(sign3(c.r),sign3(c.g),sign3(c.b)); }\n");
        /* Match xemu PGRAPH's HILO unpack. The three hemisphere variants
         * remain its documented color fallback until their reconstruction
         * equations are implemented there too. */
        EMIT("float3 dotmap_hilo_1(float4 c) {\n");
        EMIT("    uint hi_i = (uint(c.a * 255.0) << 8) | uint(c.r * 255.0);\n");
        EMIT("    uint lo_i = (uint(c.g * 255.0) << 8) | uint(c.b * 255.0);\n");
        EMIT("    return float3(float(hi_i) / 65535.0, float(lo_i) / 65535.0, 1.0);\n");
        EMIT("}\n");
        EMIT("float3 dotmap_hilo_hemisphere_d3d(float4 c) { return c.rgb; }\n");
        EMIT("float3 dotmap_hilo_hemisphere_gl(float4 c) { return c.rgb; }\n");
        EMIT("float3 dotmap_hilo_hemisphere(float4 c) { return c.rgb; }\n");
        EMIT("float3 dotmap_invalid(float4 c) { return float3(0, 0, 0); }\n\n");
    }

    /*
     * NV2A fog register: rgb carries SET_FOG_COLOR, alpha carries the
     * per-fragment fog factor.  MM3-class titles use table fog, whose
     * factor the hardware derives from eye distance; eye-space W is the
     * same quantity this translator already trusts for W-buffer depth.
     * D3DFOGMODE: 1 = EXP, 2 = EXP2, 3 = LINEAR; anything else is
     * unattenuated.  No VS-paired FOG input may be declared here.
     */
    EMIT("float xrecomp_fog_factor(float w) {\n");
    EMIT("    float coord = max(w, 0.0);\n");
    EMIT("    if (fog_mode == 3u)\n");
    EMIT("        return saturate((fog_end - coord) /\n");
    EMIT("                        max(fog_end - fog_start, 1.0e-6));\n");
    EMIT("    if (fog_mode == 1u)\n");
    EMIT("        return saturate(exp(-fog_density * coord));\n");
    EMIT("    if (fog_mode == 2u) {\n");
    EMIT("        float d = fog_density * coord;\n");
    EMIT("        return saturate(exp(-d * d));\n");
    EMIT("    }\n");
    EMIT("    return 1.0;\n");
    EMIT("}\n\n");

    /* ---- Main function ---- */
    if (state->z_perspective)
        EMIT("float4 main(PS_IN input, out float out_depth : SV_Depth)"
             " : SV_TARGET {\n");
    else
        EMIT("float4 main(PS_IN input) : SV_TARGET {\n");

    /* Initialize register file */
    EMIT("    /* Register file initialization */\n");
    EMIT("    float4 r_zero = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_c0   = c0[0];\n");
    EMIT("    float4 r_c1   = c1[0];\n");
    EMIT("    float4 r_fog  = float4(fog_color.rgb, "
         "xrecomp_fog_factor(input.pos.w));\n");

    /* Vertex colors: Xbox D3DCOLOR is BGRA in memory, the vertex shader
     * should have already swizzled to RGBA. */
    EMIT("    float4 r_v0   = input.color0;\n");
    EMIT("    float4 r_v1   = input.color1;\n");

    /* Texture-shader evaluation precedes the register combiners. */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        /* Cleared by the branches that synthesize r_t instead of sampling a
         * texture; the luminance swizzle below must not rewrite those
         * constants (it would turn (0,0,0,0) into (0,0,0,1)). */
        int sampled = 1;
        if (state->use_texture_shader) {
            uint32_t mode =
                effective_texture_shader_mode(state, i);
            if (mode == 0) {
                EMIT("    float4 r_t%d = float4(0, 0, 0, 1);\n", i);
                sampled = 0;
            } else if (mode == 1) {
                if (texture_binding_is_3d(state, i)) {
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float3((input.tc%d.xy / input.tc%d.w) * texcoord_scale[%d].xy, 0));\n",
                         i, i, i, i, i, i);
                } else {
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, (input.tc%d.xy / input.tc%d.w) * texcoord_scale[%d].xy);\n",
                         i, i, i, i, i, i);
                }
            } else if (mode == 2) {
                EMIT("    float4 r_t%d = tex%d.Sample(samp%d, (input.tc%d.xyz / input.tc%d.w) * texcoord_scale[%d].xyz);\n",
                     i, i, i, i, i, i);
            } else if (mode == 3) {
                if (state->texture_cube[i]) {
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz);\n",
                         i, i, i, i);
                } else {
                    /* A non-cube resource behind the CUBEMAP texture-shader
                     * operation is legal NV2A state. Its cube vector is
                     * projected onto a 2D face by the hardware. */
                    EMIT("    float3 cube_tc%d = input.tc%d.xyz;\n", i, i);
                    EMIT("    float3 cube_abs%d = abs(cube_tc%d);\n", i, i);
                    EMIT("    float2 cube_uv%d;\n", i);
                    EMIT("    if (cube_abs%d.x > cube_abs%d.y && cube_abs%d.x > cube_abs%d.z) {\n",
                         i, i, i, i);
                    EMIT("        cube_uv%d = cube_tc%d.x > 0.0 ? float2(-cube_tc%d.z, cube_tc%d.y) : float2(cube_tc%d.z, cube_tc%d.y);\n",
                         i, i, i, i, i, i);
                    EMIT("        cube_uv%d /= cube_abs%d.x;\n", i, i);
                    EMIT("    } else if (cube_abs%d.y > cube_abs%d.x && cube_abs%d.y > cube_abs%d.z) {\n",
                         i, i, i, i);
                    EMIT("        cube_uv%d = cube_tc%d.y > 0.0 ? float2(cube_tc%d.x, -cube_tc%d.z) : float2(cube_tc%d.x, cube_tc%d.z);\n",
                         i, i, i, i, i, i);
                    EMIT("        cube_uv%d /= cube_abs%d.y;\n", i, i);
                    EMIT("    } else {\n");
                    EMIT("        cube_uv%d = cube_tc%d.z > 0.0 ? float2(cube_tc%d.x, cube_tc%d.y) : float2(-cube_tc%d.x, cube_tc%d.y);\n",
                         i, i, i, i, i, i);
                    EMIT("        cube_uv%d /= cube_abs%d.z;\n", i, i);
                    EMIT("    }\n");
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, cube_uv%d * texcoord_scale[%d].xy);\n",
                         i, i, i, i, i);
                }
            } else if (mode == 4) {
                /* PASSTHRU exposes the interpolated texture coordinate
                 * directly to the register combiners. */
                EMIT("    float4 r_t%d = input.tc%d;\n", i, i);
                sampled = 0;
            } else if (mode == 5) {
                /*
                 * CLIPPLANE's per-component comparison mode is not carried
                 * by this state object yet. Materialize the register so the
                 * shader remains valid; clipping remains a separate semantic
                 * gap instead of a host-compiler failure.
                 */
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* CLIPPLANE */\n",
                     i);
                sampled = 0;
            } else if (mode == 6 || mode == 7) {
                /* BUMPENVMAP(_LUM): perturb this stage's texcoords by the
                 * 2x2 bump matrix applied to the input stage's signed ds/dt
                 * (sampled from .b/.g, xemu's unsigned-texture convention). */
                const float *m = state->bump_env_mat[i];
                const char *input_reg = texture_input_reg_name(state, i);
                EMIT("    float2 dsdt%d = float2(sign3(%s.b), sign3(%s.g));\n",
                     i, input_reg, input_reg);
                EMIT("    dsdt%d = float2(dot(float2(%.9g,%.9g), dsdt%d), "
                     "dot(float2(%.9g,%.9g), dsdt%d));\n",
                     i, m[0], m[2], i, m[1], m[3], i);
                if (texture_binding_is_3d(state, i)) {
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, "
                         "float3((input.tc%d.xy + dsdt%d) * "
                         "texcoord_scale[%d].xy, input.tc%d.z * "
                         "texcoord_scale[%d].z));\n",
                         i, i, i, i, i, i, i, i);
                } else {
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, "
                         "(input.tc%d.xy + dsdt%d) * "
                         "texcoord_scale[%d].xy);\n",
                         i, i, i, i, i, i);
                }
                if (mode == 7) {
                    EMIT("    r_t%d = r_t%d * (%.9g * %s.r + %.9g);\n",
                         i, i, state->bump_env_scale[i], input_reg,
                         state->bump_env_offset[i]);
                }
            } else if (mode == 8) {
                /* BRDF is undocumented on NV2A and xemu also leaves its
                 * result unimplemented. Keep a defined register value. */
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* BRDF */\n",
                     i);
                sampled = 0;
            } else if (mode == 0x09) {
                /* DOT_ST is legal only in stages 2/3 and consumes the dot
                 * result produced by the immediately preceding DOTPRODUCT
                 * stage.  Retail definitions can leave garbage in an unused
                 * texture-mode slot; never turn that into an undeclared or
                 * negative HLSL identifier. */
                if (i >= 2 && effective_texture_shader_mode(state, i - 1) ==
                                  0x11u) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float2(dot%d, dot%d) * texcoord_scale[%d].xy);\n",
                         i, i, i, i - 1, i, i);
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid DOT_ST chain */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x0A) {
                if (i >= 2) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                }
                /* DOT_ZW replaces depth on hardware. Depth replacement is
                 * tracked separately; the color register itself is zero. */
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* DOT_ZW */\n",
                     i);
                sampled = 0;
            } else if (mode == 0x0B) {
                if (i == 2 &&
                    effective_texture_shader_mode(state, 1) == 0x11u) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                    EMIT("    float dot%d_n = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i + 1, dotmap_func[state->dot_map[i + 1]],
                         texture_input_reg_name(state, i + 1));
                    EMIT("    float3 diffuse%d = float3(dot%d, dot%d, dot%d_n);\n",
                         i, i - 1, i, i);
                    if (state->texture_cube[i]) {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, diffuse%d);\n",
                             i, i, i, i);
                    } else {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, diffuse%d.xy * texcoord_scale[%d].xy);\n",
                             i, i, i, i, i);
                    }
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid DOT_RFLCT_DIFF stage */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x0C) {
                if (i == 3 &&
                    effective_texture_shader_mode(state, 1) == 0x11u &&
                    effective_texture_shader_mode(state, 2) == 0x11u) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                    EMIT("    float3 normal%d = float3(dot%d, dot%d, dot%d);\n",
                         i, i - 2, i - 1, i);
                    EMIT("    float3 eye%d = float3(input.tc%d.w, input.tc%d.w, input.tc%d.w);\n",
                         i, i - 2, i - 1, i);
                    EMIT("    float normal_len%d = max(dot(normal%d, normal%d), 1.0e-8);\n",
                         i, i, i);
                    EMIT("    float3 reflection%d = 2.0 * normal%d * dot(normal%d, eye%d) / normal_len%d - eye%d;\n",
                         i, i, i, i, i, i);
                    if (state->texture_cube[i]) {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, reflection%d);\n",
                             i, i, i, i);
                    } else {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, reflection%d.xy * texcoord_scale[%d].xy);\n",
                             i, i, i, i, i);
                    }
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid DOT_RFLCT_SPEC stage */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x11) {
                if (i == 1 || i == 2) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                }
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
                sampled = 0;
            } else if (mode == 0x0D) {
                if (i == 3 &&
                    effective_texture_shader_mode(state, 1) == 0x11u &&
                    effective_texture_shader_mode(state, 2) == 0x11u) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                    if (texture_binding_is_3d(state, i)) {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float3(dot%d, dot%d, dot%d) * texcoord_scale[%d].xyz);\n",
                             i, i, i, i - 2, i - 1, i, i);
                    } else {
                        EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float2(dot%d, dot%d) * texcoord_scale[%d].xy);\n",
                             i, i, i, i - 2, i - 1, i);
                    }
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid DOT_STR_3D chain */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x0E) {
                if (i == 3 &&
                    effective_texture_shader_mode(state, 1) == 0x11u &&
                    effective_texture_shader_mode(state, 2) == 0x11u) {
                    EMIT("    float dot%d = dot(input.tc%d.xyz, %s(%s));\n",
                         i, i, dotmap_func[state->dot_map[i]],
                         texture_input_reg_name(state, i));
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float3(dot%d, dot%d, dot%d));\n",
                         i, i, i, i - 2, i - 1, i);
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid DOT_STR_CUBE chain */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x0F || mode == 0x10) {
                int in = state->input_texture[i];
                if (i >= 1 && in >= 0 && in < i) {
                    EMIT("    float2 dependent%d = r_t%d.%s;\n",
                         i, in, mode == 0x0F ? "ar" : "gb");
                    EMIT("    float4 r_t%d = tex%d.Sample(samp%d, dependent%d * texcoord_scale[%d].xy);\n",
                         i, i, i, i, i);
                } else {
                    EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* invalid dependent stage */\n",
                         i);
                    sampled = 0;
                }
            } else if (mode == 0x12) {
                /* DOT_RFLCT_SPEC_CONST is undocumented and unimplemented by
                 * xemu. It must still define the destination register. */
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* DOT_RFLCT_SPEC_CONST */\n",
                     i);
                sampled = 0;
            } else {
                /* Reserved modes are invalid guest state, but emitting
                 * malformed host HLSL turns one bad shader into a compile
                 * storm. Fail closed with a defined zero register. */
                EMIT("    float4 r_t%d = float4(0, 0, 0, 0); /* reserved texture mode 0x%02X */\n",
                     i, mode);
                sampled = 0;
            }
        } else if (state->tex_mode[i] == NV2A_TEXMODE_NONE) {
            EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
            sampled = 0;
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float3(input.tc%d * texcoord_scale[%d].xy, 0));\n",
                 i, i, i, i, i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, float3(input.tc%d * texcoord_scale[%d].xy, 0));\n",
                 i, i, i, i, i);
        } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d * texcoord_scale[%d].xy);\n",
                 i, i, i, i, i);
        }
        /*
         * LU_IMAGE_Y16 is luminance: it uploads as R16_UNORM, whose native
         * host sample is (R,0,0,1). NV2A and xemu expose it as (R,R,R,1) to
         * both register combiners and the texture-shader DOT pipe. Keep the
         * replication on actual samples only; synthesized t-registers must
         * not acquire luminance constants.
         */
        if (sampled && state->texture_luminance[i])
            EMIT("    r_t%d = float4(r_t%d.rrr, 1.0);\n", i, i);
        if (sampled && state->color_key_mode[i] !=
                           XBOX_D3DTCOLORKEYOP_DISABLE) {
            EMIT("    uint4 color_key_bytes%d = "
                 "(uint4)round(saturate(r_t%d) * 255.0);\n", i, i);
            EMIT("    uint color_key_sample%d = "
                 "(color_key_bytes%d.a << 24) | "
                 "(color_key_bytes%d.r << 16) | "
                 "(color_key_bytes%d.g << 8) | color_key_bytes%d.b;\n",
                 i, i, i, i, i);
            EMIT("    if ((color_key_sample%d & 0x%08Xu) == "
                 "(0x%08Xu & 0x%08Xu)) {\n",
                 i, state->color_key_mask[i], state->color_key[i],
                 state->color_key_mask[i]);
            if (state->color_key_mode[i] == XBOX_D3DTCOLORKEYOP_ALPHA)
                EMIT("        r_t%d.a = 0.0;\n", i);
            else if (state->color_key_mode[i] == XBOX_D3DTCOLORKEYOP_RGBA)
                EMIT("        r_t%d = float4(0, 0, 0, 0);\n", i);
            else
                EMIT("        discard;\n");
            EMIT("    }\n");
        }
    }

    /* NV2A clears temporary-register RGB before the first general combiner.
     * R0 alpha alone is seeded from T0 when texture stage 0 is active (or
     * one when it is disabled). Copying all of T0 leaks texture data into
     * shaders which read R0 before first writing it; depth-as-Y16 mask passes
     * are especially sensitive to that. Match xemu's register-file setup. */
    if (state->use_texture_shader
            ? effective_texture_shader_mode(state, 0) != 0
            : state->tex_mode[0] != NV2A_TEXMODE_NONE)
        EMIT("    float4 r_r0 = float4(0, 0, 0, r_t0.a);\n");
    else
        EMIT("    float4 r_r0 = float4(0, 0, 0, 1.0);\n");
    EMIT("    float4 r_r1 = float4(0, 0, 0, 0);\n");
    /* Final-combiner special registers are not legal general-combiner
     * sources, but retail definitions can still encode them there. Keep the
     * generated program valid and fail closed until the final combiner
     * computes their real values. */
    EMIT("    float4 r_ef = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_v1r0sum = float4(0, 0, 0, 0);\n\n");

    /* ---- General combiner stages ---- */
    for (i = 0; i < state->num_stages; i++) {
        const NV2ACombinerInput *rgb_in  = state->stages[i].rgb_input;
        const NV2ACombinerInput *alpha_in = state->stages[i].alpha_input;
        const NV2ACombinerOutput *rgb_out = &state->stages[i].rgb_output;
        const NV2ACombinerOutput *alpha_out = &state->stages[i].alpha_output;

        EMIT("    /* ---- Stage %d ---- */\n", i);

        /* Update per-stage constants (only if this stage uses C0/C1) */
        EMIT("    r_c0 = c0[%d];\n", i);
        EMIT("    r_c1 = c1[%d];\n", i);

        /*
         * RGB path: compute AB and CD products
         *
         * AB_rgb = map(A) * map(B)    (component-wise, or dot3 if ab_dot)
         * CD_rgb = map(C) * map(D)    (component-wise, or dot3 if cd_dot)
         */
        EMIT("    {\n");

        /* AB product */
        EMIT("        float3 a_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[0], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 b_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[1], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->ab_dot) {
            EMIT("        float3 ab_rgb = float3(dot(a_rgb, b_rgb), "
                 "dot(a_rgb, b_rgb), dot(a_rgb, b_rgb));\n");
        } else {
            EMIT("        float3 ab_rgb = a_rgb * b_rgb;\n");
        }

        /* CD product */
        EMIT("        float3 c_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[2], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 d_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[3], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->cd_dot) {
            EMIT("        float3 cd_rgb = float3(dot(c_rgb, d_rgb), "
                 "dot(c_rgb, d_rgb), dot(c_rgb, d_rgb));\n");
        } else {
            EMIT("        float3 cd_rgb = c_rgb * d_rgb;\n");
        }

        /* Sum or mux. xemu/GL: condition ? CD : AB (not AB : CD).
         * MUX_MSB (flags bit0): r0.a >= 0.5; else LSB of r0.a*255. */
        if (rgb_out->mux_sum) {
            if (state->flags & 0x1u) {
                EMIT("        float3 sum_rgb = (r_r0.a >= 0.5) ? cd_rgb : ab_rgb;\n");
            } else {
                EMIT("        float3 sum_rgb = "
                     "(((uint)(r_r0.a * 255.0f) & 1u) == 1u) ? cd_rgb : ab_rgb;\n");
            }
        } else {
            EMIT("        float3 sum_rgb = ab_rgb + cd_rgb;\n");
        }

        /* Apply output mapping (scale/bias) */
        const char *omp = output_map_prefix(rgb_out->output_map);
        const char *oms = output_map_suffix(rgb_out->output_map);

        /* Write to destination registers. General-combiner stage outputs
         * clamp to [-1,1] before register writeback (NV_register_combiners);
         * without this, biased/scaled outputs escape the register range and
         * compound through later stages. */
        if (rgb_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%sab_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->ab_dst), omp, oms);
            if (rgb_out->ab_blue_to_alpha) {
                EMIT("        %s.a = clamp((%sab_rgb%s).b, -1.0, 1.0);\n",
                     reg_name(rgb_out->ab_dst), omp, oms);
            }
        }
        if (rgb_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%scd_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->cd_dst), omp, oms);
            if (rgb_out->cd_blue_to_alpha) {
                EMIT("        %s.a = clamp((%scd_rgb%s).b, -1.0, 1.0);\n",
                     reg_name(rgb_out->cd_dst), omp, oms);
            }
        }
        if (rgb_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%ssum_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->sum_dst), omp, oms);
        }

        EMIT("    }\n");

        /*
         * Alpha path: same structure but scalar operations.
         * Uses .a swizzle for all reads/writes.
         */
        EMIT("    {\n");

        EMIT("        float a_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[0], ".a", i);
        EMIT(";\n");
        EMIT("        float b_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[1], ".a", i);
        EMIT(";\n");
        EMIT("        float ab_a = a_a * b_a;\n");

        EMIT("        float c_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[2], ".a", i);
        EMIT(";\n");
        EMIT("        float d_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[3], ".a", i);
        EMIT(";\n");
        EMIT("        float cd_a = c_a * d_a;\n");

        if (alpha_out->mux_sum) {
            if (state->flags & 0x1u) {
                EMIT("        float sum_a = (r_r0.a >= 0.5) ? cd_a : ab_a;\n");
            } else {
                EMIT("        float sum_a = "
                     "(((uint)(r_r0.a * 255.0f) & 1u) == 1u) ? cd_a : ab_a;\n");
            }
        } else {
            EMIT("        float sum_a = ab_a + cd_a;\n");
        }

        /* Alpha output mapping */
        omp = output_map_prefix(alpha_out->output_map);
        oms = output_map_suffix(alpha_out->output_map);

        if (alpha_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%sab_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->ab_dst), omp, oms);
        }
        if (alpha_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%scd_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->cd_dst), omp, oms);
        }
        if (alpha_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%ssum_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->sum_dst), omp, oms);
        }

        EMIT("    }\n\n");
    }

    EMIT("    float4 result;\n");
    if (state->final_enabled) {
        /* ---- Final combiner ----
         *
         * The NV2A final combiner computes:
         *   result.rgb = D + lerp(C, B, A)
         *              = D + A*B + (1-A)*C
         *   result.a   = G (blue or alpha channel selected by G)
         *
         * Additionally, E*F is computed and made available as the EF_PROD
         * register, and V1+R0 is available as V1R0_SUM. These are computed
         * BEFORE the final combiner reads its inputs.
         */
        EMIT("    /* ---- Final Combiner ---- */\n");

        /* Compute specials: EF product and V1R0 sum */
        /* E * F product. Final-combiner C0/C1 reads use fc0/fc1. */
        EMIT("    {\n");
        EMIT("        float4 e_val = float4(");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".rgb", -1);
        EMIT(", ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".a", -1);
        EMIT(");\n");
        EMIT("        float4 f_val = float4(");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".rgb", -1);
        EMIT(", ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".a", -1);
        EMIT(");\n");
        EMIT("        r_ef = e_val * f_val;\n");
        EMIT("    }\n");

        /* V1 + R0 sum with optional complements and optional clamp. */
        {
            const char *v1_expr = state->final_inv_v1 ? "(1.0 - r_v1.rgb)" : "r_v1.rgb";
            const char *r0_expr = state->final_inv_r0 ? "(1.0 - r_r0.rgb)" : "r_r0.rgb";
            if (state->final_clamp_sum) {
                EMIT("    r_v1r0sum = float4(saturate(%s + %s), 0.0);\n\n",
                     v1_expr, r0_expr);
            } else {
                EMIT("    r_v1r0sum = float4(%s + %s, 0.0);\n\n",
                     v1_expr, r0_expr);
            }
        }

        /* Final combiner: result.rgb = D + A*B + (1-A)*C */
        EMIT("    {\n");

        /* Read final combiner inputs A, B, C, D (C0/C1 use fc0/fc1). */
        EMIT("        float3 fc_a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[0], ".rgb", -1);
        EMIT(";\n");
        EMIT("        float3 fc_b = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[1], ".rgb", -1);
        EMIT(";\n");
        EMIT("        float3 fc_c = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[2], ".rgb", -1);
        EMIT(";\n");
        EMIT("        float3 fc_d = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[3], ".rgb", -1);
        EMIT(";\n");

        /* result.rgb = D + lerp(C, B, A) = D + A*B + (1-A)*C */
        EMIT("        result.rgb = saturate(fc_d + fc_a * fc_b + (1.0 - fc_a) * fc_c);\n");

        /* G's channel bit selects alpha or blue. */
        EMIT("        result.a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[6], ".a", -1);
        EMIT(";\n");

        EMIT("    }\n\n");
    } else {
        /* Without a final combiner, the last general stage's R0 is the
         * fragment output. Do not interpret zero final-input words as a
         * combiner that returns transparent black. */
        EMIT("    /* ---- No Final Combiner: output R0 ---- */\n");
        EMIT("    result = r_r0;\n\n");
    }

    /* ---- Fog ---- */
    EMIT("    /* Fog application */\n");
    EMIT("    if (fog_enable) {\n");
    EMIT("        result.rgb = lerp(fog_color.rgb, result.rgb, r_fog.a);\n");
    EMIT("    }\n\n");

    /* ---- Alpha test ---- */
    EMIT("    /* Alpha test */\n");
    EMIT("    if (alpha_test_enable) {\n");
    EMIT("        bool alpha_pass = true;\n");
    EMIT("        if      (alpha_func == 1u) alpha_pass = false;\n");
    EMIT("        else if (alpha_func == 2u) alpha_pass = (result.a <  alpha_ref);\n");
    EMIT("        else if (alpha_func == 3u) alpha_pass = (result.a == alpha_ref);\n");
    EMIT("        else if (alpha_func == 4u) alpha_pass = (result.a <= alpha_ref);\n");
    EMIT("        else if (alpha_func == 5u) alpha_pass = (result.a >  alpha_ref);\n");
    EMIT("        else if (alpha_func == 6u) alpha_pass = (result.a != alpha_ref);\n");
    EMIT("        else if (alpha_func == 7u) alpha_pass = (result.a >= alpha_ref);\n");
    EMIT("        if (!alpha_pass) discard;\n");
    EMIT("    }\n\n");

    if (state->z_perspective) {
        /* W-buffer. SV_Position.w in a PIXEL shader is the perspective-correct
         * interpolated w -- MEASURED, not assumed: a probe shader emitting
         * saturate(pos.w) and saturate(1/pos.w) as colour gave
         * (1, 5.369e-6) on scene draws, i.e. pos.w = 186255. It is NOT 1/w.
         * (Two earlier attempts used 1.0/input.pos.w, which produced
         *  1/186255 / 2^24 = 3.2e-13 ~ 0, slamming every W fragment onto the
         *  near plane and occluding the scene.)
         * Guest w already sits in the 24-bit depth range, so the mapping is a
         * plain divide by 2^24, matching xemu's floor(zvalue)/16777216 for
         * Z24S8. Assigned after the alpha test. */
        EMIT("    out_depth = saturate(input.pos.w * (1.0 / 16777216.0));\n");
    }
    EMIT("    return result;\n");
    EMIT("}\n");

    return off;
}

#undef EMIT

/* ================================================================
 * Portable shader registry and draw integration
 * ================================================================ */

static uint32_t register_combiner_shader(const NV2ACombinerState *state)
{
    static const char prefix[] = "// XRECOMP_HLSL\n";
    char source[sizeof(prefix) + 16384];
    int length;
    memcpy(source, prefix, sizeof(prefix) - 1);
    length = d3d8_combiners_generate_hlsl(
        state, source + sizeof(prefix) - 1,
        (int)(sizeof(source) - sizeof(prefix) + 1));
    if (length < 0)
        return 0;
    source[sizeof(prefix) - 1 + (size_t)length] = '\0';
    return xgpu_plume_create_pixel_shader(source);
}

static uint32_t combiner_shader_handle(const NV2ACombinerState *state)
{
    NV2ACombinerState shader_state = combiner_shader_state(state);
    uint32_t hash = combiner_state_hash(&shader_state);
    uint32_t index = hash & (COMBINER_CACHE_SIZE - 1);
    uint32_t probe;
    CombinerCacheEntry *empty = NULL;
    for (probe = 0; probe < COMBINER_CACHE_SIZE; ++probe) {
        CombinerCacheEntry *entry =
            &g_cache[(index + probe) & (COMBINER_CACHE_SIZE - 1)];
        if (!entry->in_use) {
            empty = entry;
            break;
        }
        if (entry->hash == hash &&
            combiner_state_equal(&entry->state, &shader_state)) {
            entry->last_used_frame = g_frame_counter;
            return entry->shader_handle;
        }
    }
    if (!empty) {
        empty = &g_cache[0];
        for (probe = 1; probe < COMBINER_CACHE_SIZE; ++probe)
            if (g_cache[probe].last_used_frame < empty->last_used_frame)
                empty = &g_cache[probe];
    }
    empty->shader_handle = register_combiner_shader(state);
    if (!empty->shader_handle)
        return 0;
    empty->in_use = TRUE;
    empty->hash = hash;
    empty->state = shader_state;
    empty->last_used_frame = g_frame_counter;
    return empty->shader_handle;
}

HRESULT d3d8_combiners_init(void)
{
    int i;
    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    memset(&g_constants, 0, sizeof(g_constants));
    for (i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        g_texture_dimensions[i] = 2;
        g_color_key_mask[i] = UINT32_MAX;
    }
    memset(g_texture_cube, 0, sizeof(g_texture_cube));
    memset(g_texture_luminance, 0, sizeof(g_texture_luminance));
    memset(g_bump_env, 0, sizeof(g_bump_env));
    memset(g_color_key_mode, 0, sizeof(g_color_key_mode));
    memset(g_color_key, 0, sizeof(g_color_key));
    g_z_perspective = 0;
    g_ps_token = 0;
    g_definition_active = FALSE;
    memset(g_definition, 0, sizeof(g_definition));
    g_dirty = TRUE;
    g_constants_dirty = TRUE;
    g_current_shader_handle = 0;
    g_frame_counter = 0;
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    memset(&g_constants, 0, sizeof(g_constants));
    memset(g_texture_dimensions, 0, sizeof(g_texture_dimensions));
    memset(g_texture_cube, 0, sizeof(g_texture_cube));
    memset(g_texture_luminance, 0, sizeof(g_texture_luminance));
    memset(g_bump_env, 0, sizeof(g_bump_env));
    memset(g_color_key_mode, 0, sizeof(g_color_key_mode));
    memset(g_color_key, 0, sizeof(g_color_key));
    memset(g_color_key_mask, 0, sizeof(g_color_key_mask));
    g_z_perspective = 0;
    g_definition_active = FALSE;
    memset(g_definition, 0, sizeof(g_definition));
    g_dirty = TRUE;
    g_constants_dirty = TRUE;
    g_current_shader_handle = 0;
}

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token || g_definition_active) {
        g_ps_token = token;
        g_definition_active = FALSE;
        g_dirty = TRUE;
        g_constants_dirty = TRUE;
    }
}

void d3d8_combiners_set_definition(const DWORD definition[60])
{
    if (!definition) {
        d3d8_combiners_set_pixel_shader(0);
        return;
    }
    if (!g_definition_active ||
        memcmp(g_definition, definition, sizeof(g_definition)) != 0) {
        memcpy(g_definition, definition, sizeof(g_definition));
        g_definition_active = TRUE;
        g_ps_token = 1;
        g_dirty = TRUE;
        g_constants_dirty = TRUE;
    }
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
    g_constants_dirty = TRUE;
}

void d3d8_combiners_mark_constants_dirty(void)
{
    g_constants_dirty = TRUE;
}

void d3d8_combiners_set_texture_binding(
    UINT stage, UINT dimensionality, BOOL cube, BOOL luminance, UINT format)
{
    uint8_t normalized_dimension;
    uint8_t normalized_cube;
    uint8_t normalized_luminance;
    uint32_t color_key_mask;
    if (stage >= NV2A_MAX_TEXTURES)
        return;
    normalized_dimension = dimensionality == 3u ? 3u : 2u;
    normalized_cube = cube ? 1u : 0u;
    normalized_luminance = luminance ? 1u : 0u;
    color_key_mask = xbox_d3d8_color_key_mask(format);
    if (g_texture_dimensions[stage] != normalized_dimension ||
        g_texture_cube[stage] != normalized_cube ||
        g_texture_luminance[stage] != normalized_luminance ||
        g_color_key_mask[stage] != color_key_mask) {
        g_texture_dimensions[stage] = normalized_dimension;
        g_texture_cube[stage] = normalized_cube;
        g_texture_luminance[stage] = normalized_luminance;
        g_color_key_mask[stage] = color_key_mask;
        g_dirty = TRUE;
    }
}

BOOL d3d8_combiners_set_bump_env(UINT stage, UINT type, DWORD value)
{
    int component = xbox_d3d8_bump_env_component(type);
    float converted;

    if (stage >= NV2A_MAX_TEXTURES || component < 0)
        return FALSE;
    memcpy(&converted, &value, sizeof(converted));
    if (g_bump_env[stage][component] != converted) {
        g_bump_env[stage][component] = converted;
        g_dirty = TRUE;
    }
    return TRUE;
}

BOOL d3d8_combiners_set_color_key(UINT stage, DWORD value)
{
    if (stage >= NV2A_MAX_TEXTURES)
        return FALSE;
    if (g_color_key[stage] != value) {
        g_color_key[stage] = value;
        g_dirty = TRUE;
    }
    return TRUE;
}

BOOL d3d8_combiners_set_color_key_mode(UINT stage, UINT mode)
{
    if (stage >= NV2A_MAX_TEXTURES || mode > XBOX_D3DTCOLORKEYOP_KILL)
        return FALSE;
    if (g_color_key_mode[stage] != mode) {
        g_color_key_mode[stage] = (uint8_t)mode;
        g_dirty = TRUE;
    }
    return TRUE;
}

void d3d8_combiners_set_z_perspective(BOOL enabled)
{
    uint8_t normalized = enabled ? 1u : 0u;
    if (g_z_perspective != normalized) {
        g_z_perspective = normalized;
        g_dirty = TRUE;
    }
}

BOOL d3d8_combiners_prepare_draw(void)
{
    const DWORD *rs;
    int i;
    if (!g_ps_token) {
        /* SetPixelShader(0) already clears D3D8-owned shader state. Leave the
         * renderer untouched here so native PGRAPH can supply its own
         * combiner handle before entering the shared draw path. */
        return FALSE;
    }
    rs = d3d8_GetRenderStates();
    if (!rs)
        return FALSE;
    if (g_dirty) {
        if (g_definition_active)
            d3d8_combiners_parse_definition(
                g_definition, &g_combiner_state);
        else
            d3d8_combiners_parse_token(
                g_ps_token, rs, &g_combiner_state);
        for (i = 0; i < NV2A_MAX_TEXTURES; ++i) {
            int bump_stage = g_definition_active ? i : i + 1;

            g_combiner_state.texture_dimensions[i] =
                g_texture_dimensions[i];
            g_combiner_state.texture_cube[i] = g_texture_cube[i];
            g_combiner_state.texture_luminance[i] =
                g_texture_luminance[i];
            g_combiner_state.color_key_mode[i] = g_color_key_mode[i];
            g_combiner_state.color_key[i] = g_color_key[i];
            g_combiner_state.color_key_mask[i] = g_color_key_mask[i];
            if (bump_stage < NV2A_MAX_TEXTURES) {
                memcpy(g_combiner_state.bump_env_mat[bump_stage],
                       g_bump_env[i],
                       sizeof(g_combiner_state.bump_env_mat[bump_stage]));
                g_combiner_state.bump_env_scale[bump_stage] =
                    g_bump_env[i][4];
                g_combiner_state.bump_env_offset[bump_stage] =
                    g_bump_env[i][5];
            }
        }
        g_combiner_state.z_perspective = g_z_perspective;
        g_current_shader_handle =
            combiner_shader_handle(&g_combiner_state);
        if (!g_current_shader_handle)
            return FALSE;
        g_dirty = FALSE;
        g_constants_dirty = TRUE;
    }
    if (!g_current_shader_handle)
        return FALSE;
    if (g_constants_dirty) {
        int final_stage = g_combiner_state.num_stages > 0
            ? g_combiner_state.num_stages - 1 : 0;
        memset(&g_constants, 0, sizeof(g_constants));
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; ++i) {
            d3dcolor_to_float4(
                g_definition_active ? g_combiner_state.c0[i]
                                    : rs[D3DRS_PSCONSTANT0_0 + i],
                g_constants.c0[i]);
            d3dcolor_to_float4(
                g_definition_active ? g_combiner_state.c1[i]
                                    : rs[D3DRS_PSCONSTANT1_0 + i],
                g_constants.c1[i]);
        }
        d3dcolor_to_float4(
            g_definition_active ? g_combiner_state.final_c0
                                : rs[D3DRS_PSCONSTANT0_0 + final_stage],
            g_constants.final_c0);
        d3dcolor_to_float4(
            g_definition_active ? g_combiner_state.final_c1
                                : rs[D3DRS_PSCONSTANT1_0 + final_stage],
            g_constants.final_c1);
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], g_constants.fog_color);
        g_constants.alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        g_constants.alpha_func = rs[D3DRS_ALPHAFUNC];
        g_constants.alpha_test_enable =
            rs[D3DRS_ALPHATESTENABLE] ? 1u : 0u;
        g_constants.fog_enable = rs[D3DRS_FOGENABLE] ? 1u : 0u;
        g_constants.fog_mode = rs[D3DRS_FOGTABLEMODE];
        memcpy(&g_constants.fog_start, &rs[D3DRS_FOGSTART],
               sizeof(g_constants.fog_start));
        memcpy(&g_constants.fog_end, &rs[D3DRS_FOGEND],
               sizeof(g_constants.fog_end));
        memcpy(&g_constants.fog_density, &rs[D3DRS_FOGDENSITY],
               sizeof(g_constants.fog_density));
        g_constants_dirty = FALSE;
    }
    /* Reassert through the shared backend because native PGRAPH work may have
     * changed the active constants between D3D8 draws.  Plume suppresses an
     * identical assignment without losing that cross-owner correctness. */
    xgpu_plume_set_combiner_consts_ex((const float *)&g_constants, 21);
    xgpu_plume_set_active_ps(g_current_shader_handle);
    ++g_frame_counter;
    return TRUE;
}
