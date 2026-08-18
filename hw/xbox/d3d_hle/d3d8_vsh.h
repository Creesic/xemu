/**
 * NV2A Vertex Shader Microcode to HLSL Translator
 *
 * The Xbox NV2A GPU has a programmable vertex shader unit compatible with
 * (and extending) the original GeForce3/4 vertex shader architecture.
 * Games upload sequences of 128-bit microcode instructions via
 * D3DDevice_CreateVertexShader(). At draw time, the NV2A executes
 * these instructions in its vertex shader pipeline.
 *
 * This module translates NV2A vertex shader microcode into HLSL source
 * code for runtime compilation by the Plume shader adapter.
 *
 * NV2A Vertex Shader Architecture:
 *
 *   Registers:
 *     v0  - v15   Input vertex attribute registers (read-only)
 *     R0  - R11   Temporary registers (read/write)
 *     R12         Aliased to oPos (output position)
 *     c0  - c191  Constant registers (set by SetVertexShaderConstant)
 *     a0          Address register (integer, for indexed c[] access)
 *     oPos        Output position (= R12)
 *     oD0, oD1    Output diffuse / specular color
 *     oFog        Output fog factor
 *     oPts        Output point size
 *     oB0, oB1    Output back-face diffuse / specular
 *     oT0 - oT3   Output texture coordinates
 *
 *   Execution Units (per instruction slot, execute in parallel):
 *     MAC (Multiply-Accumulate):
 *       NOP, MOV, MUL, ADD, MAD, DP3, DP4, DPH, DST, MIN, MAX,
 *       SLT, SGE, ARL
 *     ILU (Inverse Logic Unit):
 *       NOP, MOV, RCP, RCC, RSQ, EXP, LOG, LIT
 *
 *   Programs are up to 136 instruction slots.
 *   Each slot is 128 bits (4 DWORDs) encoding both MAC and ILU ops.
 *
 * References:
 *   - envytools NV20 vertex shader documentation
 *   - xemu NV2A vertex shader implementation
 *   - Xbox SDK D3D vertex shader programming guide
 *   - US Patent 7,002,588 (Microsoft/Nvidia vertex shader architecture)
 */

#ifndef XBOXRECOMP_D3D8_VSH_H
#define XBOXRECOMP_D3D8_VSH_H

#include <stddef.h>
#include <stdint.h>
#include "d3d8_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Vertex Shader Constants
 * ================================================================ */

/** Maximum program length in 128-bit instruction slots. */
#define NV2A_VS_MAX_INSTRUCTIONS    136

/** Number of constant registers (c0 - c191). */
#define NV2A_VS_MAX_CONSTANTS       192

/** Number of input attribute registers (v0 - v15). */
#define NV2A_VS_MAX_INPUTS          16

/** Number of temporary registers (R0 - R11, plus R12 = oPos). */
#define NV2A_VS_MAX_TEMPS           13

/** Maximum number of shader programs that can be stored. */
#define NV2A_VS_MAX_SLOTS           128

/** Internal host handles occupy one bounded range above the FVF namespace. */
#define NV2A_VS_HANDLE_BASE          0x10000u
#define NV2A_VS_HANDLE_LIMIT         (NV2A_VS_HANDLE_BASE + NV2A_VS_MAX_SLOTS)

/** Shader cache size (hashed microcode -> compiled shader). */
#define NV2A_VS_CACHE_SIZE          64

/** Bounded token count accepted from an Xbox D3D vertex declaration. */
#define NV2A_VS_MAX_DECLARATION_DWORDS 256

/* ================================================================
 * NV2A VS Instruction Encoding (128 bits = 4 DWORDs)
 *
 * Each instruction is composed of 4 x 32-bit words:
 *
 * Word 0 (ILU/misc):
 *   [31:29] - Reserved / type
 *   [28:25] - ILU opcode (4 bits)
 *   [24:21] - MAC opcode (4 bits)
 *   [20:13] - Const index (8 bits) - constant register selector
 *   [12:9]  - Input reg (v#) (4 bits) - which v0-v15 register
 *   [8]     - Source A negate
 *   [7:4]   - Source A register type + index (mux field A)
 *   [3:0]   - Source A swizzle X component (2 bits in combined field)
 *
 * Word 1 (Source A/B):
 *   [31:24] - Source A swizzle (Y, Z, W components) + remaining bits
 *   [23:16] - Source B register select + negate + type
 *   [15:0]  - Source B swizzle + remaining fields
 *
 * Word 2 (Source C / MAC dest):
 *   [31:16] - Source C register select + negate + swizzle
 *   [15:12] - MAC dest temp register index
 *   [11:8]  - MAC dest write mask (xyzw)
 *   [7:3]   - MAC dest output register mux
 *   [2:0]   - Remaining source C bits
 *
 * Word 3 (ILU dest / final):
 *   [31:28] - ILU dest temp register index
 *   [27:24] - ILU dest write mask (xyzw)
 *   [23:19] - ILU dest output register mux
 *   [18:1]  - Reserved / other fields
 *   [0]     - Final instruction flag (1 = last instruction in program)
 *
 * NOTE: The exact bit layout follows the xemu/envytools conventions.
 * The fields below are extracted using shift-and-mask operations
 * matching the NV2A hardware encoding.
 * ================================================================ */

/* ================================================================
 * Opcode Enumerations
 * ================================================================ */

/**
 * MAC unit opcodes.
 *
 * The MAC unit handles multiply-accumulate class operations.
 * It reads up to 3 source operands (A, B, C) and writes one destination.
 */
typedef enum NV2AVshMacOp {
    NV2A_VSH_MAC_NOP = 0,   /* No operation */
    NV2A_VSH_MAC_MOV = 1,   /* dst = A */
    NV2A_VSH_MAC_MUL = 2,   /* dst = A * B */
    NV2A_VSH_MAC_ADD = 3,   /* dst = A + C */
    NV2A_VSH_MAC_MAD = 4,   /* dst = A * B + C */
    NV2A_VSH_MAC_DP3 = 5,   /* dst = dot3(A.xyz, B.xyz) */
    NV2A_VSH_MAC_DPH = 6,   /* dst = dot3(A.xyz, B.xyz) + B.w */
    NV2A_VSH_MAC_DP4 = 7,   /* dst = dot4(A, B) */
    NV2A_VSH_MAC_DST = 8,   /* dst = distance vector */
    NV2A_VSH_MAC_MIN = 9,   /* dst = min(A, B) */
    NV2A_VSH_MAC_MAX = 10,  /* dst = max(A, B) */
    NV2A_VSH_MAC_SLT = 11,  /* dst = (A < B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_SGE = 12,  /* dst = (A >= B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_ARL = 13,  /* a0.x = floor(A.x) */
    NV2A_VSH_MAC_COUNT = 14,
} NV2AVshMacOp;

/**
 * ILU unit opcodes.
 *
 * The ILU unit handles transcendental/reciprocal operations.
 * It reads source operand C and writes one destination.
 * The ILU operates in parallel with the MAC unit.
 */
typedef enum NV2AVshIluOp {
    NV2A_VSH_ILU_NOP = 0,   /* No operation */
    NV2A_VSH_ILU_MOV = 1,   /* dst = C */
    NV2A_VSH_ILU_RCP = 2,   /* dst = 1.0 / C.x (scalar, replicated) */
    NV2A_VSH_ILU_RCC = 3,   /* dst = clamp(1.0/C.x, 5.42e-36, 1.884e+19) */
    NV2A_VSH_ILU_RSQ = 4,   /* dst = 1.0 / sqrt(abs(C.x)) */
    NV2A_VSH_ILU_EXP = 5,   /* dst = exp2(C.x) */
    NV2A_VSH_ILU_LOG = 6,   /* dst = log2(abs(C.x)) */
    NV2A_VSH_ILU_LIT = 7,   /* dst = lighting helper */
    NV2A_VSH_ILU_COUNT = 8,
} NV2AVshIluOp;

/**
 * Source operand register types.
 *
 * Each source operand selects from one of three register banks.
 */
typedef enum NV2AVshRegType {
    NV2A_VSH_REG_TEMP   = 0,  /* R0-R11 (R12 = oPos alias) */
    NV2A_VSH_REG_INPUT  = 1,  /* v0-v15 */
    NV2A_VSH_REG_CONST  = 2,  /* c0-c191 (may be indexed via a0) */
    NV2A_VSH_REG_ZERO   = 3,  /* Disabled source mux reads as zero */
    NV2A_VSH_REG_COUNT  = 4,
} NV2AVshRegType;

/**
 * Output register selectors.
 *
 * When a MAC/ILU destination targets an output register, these
 * identify which output. If the mux value is 0xFF, the write
 * goes only to a temp register.
 */
typedef enum NV2AVshOutputReg {
    NV2A_VSH_OUT_POS  = 0,   /* oPos (clip-space position) */
    NV2A_VSH_OUT_D0   = 3,   /* oD0 (diffuse color) */
    NV2A_VSH_OUT_D1   = 4,   /* oD1 (specular color) */
    NV2A_VSH_OUT_FOG  = 5,   /* oFog (fog factor) */
    NV2A_VSH_OUT_PTS  = 6,   /* oPts (point size) */
    NV2A_VSH_OUT_B0   = 7,   /* oB0 (back diffuse) */
    NV2A_VSH_OUT_B1   = 8,   /* oB1 (back specular) */
    NV2A_VSH_OUT_T0   = 9,   /* oT0 (texcoord 0) */
    NV2A_VSH_OUT_T1   = 10,  /* oT1 (texcoord 1) */
    NV2A_VSH_OUT_T2   = 11,  /* oT2 (texcoord 2) */
    NV2A_VSH_OUT_T3   = 12,  /* oT3 (texcoord 3) */
    NV2A_VSH_OUT_NONE = 0xFF, /* No output register write */
} NV2AVshOutputReg;

/* ================================================================
 * Parsed Instruction Representation
 * ================================================================ */

/**
 * Swizzle encoding for one component.
 * Each component selector picks from {x=0, y=1, z=2, w=3}.
 */
typedef struct NV2AVshSwizzle {
    uint8_t x;  /* 0=x, 1=y, 2=z, 3=w */
    uint8_t y;
    uint8_t z;
    uint8_t w;
} NV2AVshSwizzle;

/**
 * A fully decoded source operand.
 */
typedef struct NV2AVshSrcOperand {
    NV2AVshRegType  reg_type;    /* TEMP, INPUT, or CONST */
    int             reg_index;   /* Register number within the bank */
    int             negate;      /* 1 = negate the value */
    NV2AVshSwizzle  swizzle;     /* Per-component swizzle */
    int             rel_addr;    /* 1 = use a0.x relative addressing (CONST only) */
} NV2AVshSrcOperand;

/**
 * A fully decoded destination operand.
 */
typedef struct NV2AVshDstOperand {
    int               temp_reg;    /* Temp register index (0-12), or -1 if none */
    NV2AVshOutputReg  output_reg;  /* Output register, or NV2A_VSH_OUT_NONE */
    uint8_t           temp_write_mask;   /* bit3=x, bit2=y, bit1=z, bit0=w */
    uint8_t           output_write_mask; /* independent NV2A output mask */
} NV2AVshDstOperand;

/**
 * A fully decoded NV2A vertex shader instruction.
 *
 * Each 128-bit instruction slot can encode both a MAC operation
 * and an ILU operation that execute in parallel. Either (or both)
 * may be NOP.
 */
typedef struct NV2AVshInstruction {
    /* MAC unit */
    NV2AVshMacOp    mac_op;
    NV2AVshSrcOperand mac_src[3];  /* A, B, C */
    NV2AVshDstOperand mac_dst;

    /* ILU unit */
    NV2AVshIluOp    ilu_op;
    NV2AVshSrcOperand ilu_src;     /* C (ILU only reads source C) */
    NV2AVshDstOperand ilu_dst;

    /* Constant register index (shared) */
    int             const_index;

    /* Input register index v# (shared) */
    int             input_index;

    /* Final instruction flag */
    int             is_final;
} NV2AVshInstruction;

/**
 * A complete parsed vertex shader program.
 */
typedef struct NV2AVshProgram {
    NV2AVshInstruction  insns[NV2A_VS_MAX_INSTRUCTIONS];
    int                 length;     /* Number of instructions */
    int                 valid;      /* Decoder accepted all encoded writes */

    /* Bitmask of input registers read (v0-v15). Bit N = vN is used.
     * Used to determine the required input layout. */
    uint16_t            inputs_read;

    /* Bitmask of output registers written (oPos/oD0/.../oT3). Bit N uses
     * NV2AVshOutputReg's numeric value. The fixed-function pixel fallback
     * uses this to distinguish authored texture coordinates from its
     * initialized-but-unwritten oT0 value. */
    uint16_t            outputs_written;
} NV2AVshProgram;

/**
 * Parsed Xbox D3D8 vertex declaration.
 *
 * Xbox declarations assign each v0-v15 input register to a stream, a byte
 * offset, and an NV2A data type. The shader translator retains this metadata
 * with the microcode so the host input assembler reads the original packed
 * vertex layout instead of guessing one from the registers used by the
 * program.
 */
typedef struct NV2AVshDeclaration {
    uint16_t attributes_present;
    uint8_t stream[NV2A_VS_MAX_INPUTS];
    uint8_t format[NV2A_VS_MAX_INPUTS];
    uint16_t offset[NV2A_VS_MAX_INPUTS];
    int valid;
} NV2AVshDeclaration;

/* ================================================================
 * Shader Slot (stored microcode)
 * ================================================================ */

/**
 * A stored vertex shader program slot.
 *
 * Created by CreateVertexShader(), indexed by handle.
 * The microcode is stored as raw DWORDs; parsing and compilation
 * are deferred until the shader is first used in a draw call.
 */
typedef struct NV2AVshSlot {
    DWORD   microcode[NV2A_VS_MAX_INSTRUCTIONS * 4]; /* Raw 128-bit instructions */
    NV2AVshDeclaration declaration;
    NV2AVshProgram *parsed_program; /* Lazily decoded, shared by draw helpers */
    int     length;         /* Number of instructions */
    int     in_use;         /* 1 if this slot is allocated */
} NV2AVshSlot;

/* ================================================================
 * VS Constant Buffer Layout (HLSL)
 *
 * Uploaded to register(b9) so it doesn't conflict with the
 * fixed-function transform CB at b0.
 *
 * Must be 16-byte aligned and match the HLSL cbuffer declaration.
 * ================================================================ */

typedef struct NV2AVSConstants {
    float c[NV2A_VS_MAX_CONSTANTS][4];  /* 192 float4 constants */
} NV2AVSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the vertex shader translator.
 * Resets stored programs and constants.
 */
HRESULT d3d8_vsh_init(void);

/**
 * Shut down the vertex shader translator.
 * Releases stored translator state.
 */
void d3d8_vsh_shutdown(void);

/**
 * Store a vertex shader program (CreateVertexShader).
 *
 * Copies the microcode into an internal slot. The shader is not
 * compiled until first use.
 *
 * @param microcode   Pointer to the 128-bit instruction array (4 DWORDs each)
 * @param num_insns   Number of instructions
 * @param declaration Parsed Xbox vertex declaration, or NULL when unavailable
 * @param out_handle  Receives a handle in the bounded NV2A_VS_HANDLE range
 * @return S_OK on success
 */
HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                const NV2AVshDeclaration *declaration,
                                DWORD *out_handle);

/**
 * Parse an Xbox D3D8 tokenized vertex declaration.
 *
 * @param tokens      Declaration DWORDs, terminated by 0xFFFFFFFF
 * @param max_dwords  Maximum readable DWORDs at tokens
 * @param declaration Receives the register/stream/format/offset mapping
 * @return TRUE for a complete supported declaration, FALSE if malformed
 */
BOOL d3d8_vsh_parse_declaration(const DWORD *tokens, size_t max_dwords,
                                NV2AVshDeclaration *declaration);

/**
 * Delete a previously created vertex shader.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @return S_OK on success
 */
HRESULT d3d8_vsh_delete_shader(DWORD handle);

/**
 * Set a vertex shader constant register.
 *
 * @param start_reg  First register index (0-191)
 * @param data       Pointer to float4 data (4 floats per register)
 * @param count      Number of float4 registers to set
 */
void d3d8_vsh_set_constant(int start_reg, const float *data, int count);

/**
 * Update one persistent v-register value used when a declaration omits that
 * input. This mirrors the NV2A immediate-mode attribute latch.
 */
void d3d8_vsh_set_vertex_data4f(uint32_t reg, const float *value);

/**
 * Execute a programmable shader over a raw interleaved vertex array and
 * return the minimum/maximum authored screen-space oPos.x. The helper is used
 * only while a logical UI canvas is active, allowing the renderer to choose a
 * single rigid left/center/right transform for the complete draw.
 */
BOOL d3d8_vsh_calculate_position_bounds(DWORD handle,
                                         const void *vertices,
                                         uint32_t vertex_count,
                                         uint32_t stride,
                                         float *minimum_x,
                                         float *maximum_x);

/**
 * Check if a shader handle refers to a programmable vertex shader
 * (as opposed to an FVF code).
 *
 * FVF texture-coordinate-size fields also occupy bits 16 and above, so a
 * broad `handle > 0xFFFF` test misclassifies valid fixed-function layouts.
 * Host-created shaders use only [NV2A_VS_HANDLE_BASE,
 * NV2A_VS_HANDLE_LIMIT).
 */
BOOL d3d8_vsh_is_programmable(DWORD handle);

/**
 * Prepare for a draw call using a programmable vertex shader.
 *
 * - Parses microcode if not yet parsed
 * - Generates HLSL and registers it with Plume if not cached
 * - Snapshots the vertex constant buffer
 *
 * @param handle  The active vertex shader handle
 * @return TRUE if a programmable VS was bound, FALSE on fallback
 */
BOOL d3d8_vsh_prepare_draw(DWORD handle);

/**
 * Parse NV2A vertex shader microcode into intermediate representation.
 *
 * @param microcode  Raw instruction data (4 DWORDs per instruction)
 * @param num_insns  Number of instructions
 * @param program    Output parsed program
 */
void d3d8_vsh_parse(const DWORD *microcode, int num_insns,
                     NV2AVshProgram *program);

/**
 * Generate HLSL vertex shader source from parsed program.
 *
 * @param program   Parsed program
 * @param buf       Output buffer for HLSL source, or NULL for a sizing pass
 * @param bufsize   Size of output buffer, or zero for a sizing pass
 * @return Required characters excluding the terminator, or -1 on error.
 *   When the return value is >= bufsize, the supplied buffer was truncated.
 */
/**
 * @param vertex_format  Optional array of 16 NV2A vertex attribute format
 *   dwords (NV097_SET_VERTEX_DATA_ARRAY_FORMAT). When NULL, inputs are declared
 *   as float4 (the CPU fallback decodes attributes first). When provided, the
 *   VS_IN struct and input aliases are made format-aware so the GPU input
 *   assembler can feed raw vertex data directly (S32K/CMP are read as integers
 *   and unpacked in HLSL; F/UB/S1 are read via a matching DXGI format). The
 *   Plume input layout must bind DXGI formats matching this mapping.
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            const uint32_t *vertex_format,
                            char *buf, int bufsize);

/**
 * Generate a one-thread compute shader that executes the same translated
 * program against already-decoded v0-v15 input registers. This is used by the
 * runtime differential harness to compare raw GPU output registers with the
 * working CPU interpreter without involving vertex-buffer format conversion.
 *
 * Bindings:
 *   t0: 16 float4 input registers
 *   u1: 16 float4 output registers
 *   b9: 192 float4 NV2A constants
 */
int d3d8_vsh_generate_compute_hlsl(const NV2AVshProgram *program,
                                   char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_H */
