/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source for the Plume runtime compiler.
 *
 * The translation pipeline is:
 *   1. Parse: 128-bit instruction words -> NV2AVshInstruction structs
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Register: hand source and input metadata to the Plume adapter
 *
 * The generated HLSL uses:
 *   - cbuffer at b9: 192 float4 constants (c0-c191)
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "plume/plume_host.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

/* ================================================================
 * NV2A Instruction Bit Field Extraction
 *
 * Each instruction is 128 bits stored as 4 DWORDs (word[0..3]).
 * The following macros extract individual fields.
 *
 * Bit layout follows the envytools / xemu conventions:
 *
 * word[0] bits:
 *   [3:0]   = (unused / type marker)
 *   [24:21] = MAC opcode
 *   [28:25] = ILU opcode
 *   [20:13] = Constant register index
 *   [12:9]  = Input register index (v0-v15)
 *   [8]     = Source A negate
 *   [7:6]   = Source A register type
 *   [5:2]   = Source A temp register index (high bits)
 *
 * word[1] bits:
 *   [31:26] = Source A temp reg index (low bit) + swizzle X,Y
 *   [25:24] = Source A swizzle Z
 *   [23:22] = Source A swizzle W
 *   [21]    = Source B negate
 *   [20:19] = Source B register type
 *   [18:15] = Source B temp register index
 *   [14:13] = Source B swizzle X
 *   [12:11] = Source B swizzle Y
 *   [10:9]  = Source B swizzle Z
 *   [8:7]   = Source B swizzle W
 *   [6]     = Source C negate
 *   [5:4]   = Source C register type
 *   [3:0]   = Source C temp register index (high bits)
 *
 * word[2] bits:
 *   [31:28] = Source C temp reg index (low bits)
 *   [27:26] = Source C swizzle X
 *   [25:24] = Source C swizzle Y
 *   [23:22] = Source C swizzle Z
 *   [21:20] = Source C swizzle W
 *   [19:16] = MAC dest temp register index
 *   [15:12] = MAC dest write mask
 *   [11:3]  = MAC dest output register + mux
 *   [2:0]   = ILU dest fields (high bits)
 *
 * word[3] bits:
 *   [31:28] = ILU dest temp register index
 *   [27:24] = ILU dest write mask
 *   [23:14] = ILU dest output register + mux
 *   [13]    = Relative addressing flag (a0.x)
 *   ...
 *   [0]     = Final instruction flag
 *
 * NOTE: The exact bit positions below are derived from the xemu
 * NV2A vertex shader decoder. Different references may number
 * bits differently (MSB-first vs LSB-first within the 128-bit
 * word). We use the convention where word[0] bit 0 is the LSB.
 * ================================================================ */

/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;
static BOOL g_vsh_registered[NV2A_VS_MAX_SLOTS];
static float g_vsh_vertex_data[NV2A_VS_MAX_INPUTS][4];

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static int decode_constant_index(uint32_t encoded)
{
    int bank = (int)((encoded >> 5) & 7) - 3;
    return bank * 32 + (int)(encoded & 31) + 96;
}

static int decode_temp_index(uint32_t encoded)
{
    return encoded == 12 || encoded < 13 ? (int)encoded : 0;
}

static void parse_source(uint32_t mux, uint32_t reg, uint32_t negate,
                         uint32_t swz_x, uint32_t swz_y,
                         uint32_t swz_z, uint32_t swz_w,
                         int input_index, int const_index, int rel_addr,
                         NV2AVshSrcOperand *src)
{
    src->negate = (int)negate;
    src->swizzle.x = (uint8_t)swz_x;
    src->swizzle.y = (uint8_t)swz_y;
    src->swizzle.z = (uint8_t)swz_z;
    src->swizzle.w = (uint8_t)swz_w;
    src->rel_addr = 0;

    switch (mux) {
    case 1:
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = decode_temp_index(reg);
        break;
    case 2:
        src->reg_type  = NV2A_VSH_REG_INPUT;
        src->reg_index = input_index;
        break;
    case 3:
        src->reg_type  = NV2A_VSH_REG_CONST;
        src->reg_index = const_index;
        src->rel_addr = rel_addr;
        break;
    default:
        src->reg_type  = NV2A_VSH_REG_ZERO;
        src->reg_index = 0;
        break;
    }
}

static NV2AVshOutputReg decode_output_mux(uint32_t mux_val)
{
    /* The output register mux field encodes which output register.
     * The low nibble gives the output type. */
    uint32_t out_idx = mux_val & 0xF;
    switch (out_idx) {
    case 0:  return NV2A_VSH_OUT_POS;
    case 3:  return NV2A_VSH_OUT_D0;
    case 4:  return NV2A_VSH_OUT_D1;
    case 5:  return NV2A_VSH_OUT_FOG;
    case 6:  return NV2A_VSH_OUT_PTS;
    case 7:  return NV2A_VSH_OUT_B0;
    case 8:  return NV2A_VSH_OUT_B1;
    case 9:  return NV2A_VSH_OUT_T0;
    case 10: return NV2A_VSH_OUT_T1;
    case 11: return NV2A_VSH_OUT_T2;
    case 12: return NV2A_VSH_OUT_T3;
    default: return NV2A_VSH_OUT_NONE;
    }
}

void d3d8_vsh_parse(const DWORD *microcode, int num_insns,
                     NV2AVshProgram *program)
{
    int i;
    memset(program, 0, sizeof(*program));
    program->inputs_read = 0;
    program->valid = 1;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    for (i = 0; i < num_insns; i++) {
        const DWORD *insn = &microcode[i * 4];
        NV2AVshInstruction *inst = &program->insns[i];
        uint32_t output_mask;
        uint32_t output_address;
        uint32_t out_reg;
        int rel_addr;

        /* NV2A stores the executable fields in words 1-3. Word 0 is unused
         * for the programs MM3 uploads. Keep these expressions identical to
         * the working CPU interpreter in nv2a_pgraph.c. */
        inst->mac_op = (NV2AVshMacOp)((insn[1] >> 21) & 15);
        inst->ilu_op = (NV2AVshIluOp)((insn[1] >> 25) & 7);

        /* Shared constant and input register indices */
        inst->const_index = decode_constant_index((insn[1] >> 13) & 0xFF);
        inst->input_index = (int)((insn[1] >> 9) & 15);
        rel_addr = (insn[3] & 2u) != 0;

        /* Parse source operands A, B, C */
        parse_source((insn[2] >> 26) & 3, (insn[2] >> 28) & 15,
                     (insn[1] >> 8) & 1,
                     (insn[1] >> 6) & 3, (insn[1] >> 4) & 3,
                     (insn[1] >> 2) & 3, insn[1] & 3,
                     inst->input_index, inst->const_index, rel_addr,
                     &inst->mac_src[0]);

        parse_source((insn[2] >> 11) & 3, (insn[2] >> 13) & 15,
                     (insn[2] >> 25) & 1,
                     (insn[2] >> 23) & 3, (insn[2] >> 21) & 3,
                     (insn[2] >> 19) & 3, (insn[2] >> 17) & 3,
                     inst->input_index, inst->const_index, rel_addr,
                     &inst->mac_src[1]);

        parse_source((insn[3] >> 28) & 3,
                     ((insn[2] & 3) << 2) | (insn[3] >> 30),
                     (insn[2] >> 10) & 1,
                     (insn[2] >> 8) & 3, (insn[2] >> 6) & 3,
                     (insn[2] >> 4) & 3, (insn[2] >> 2) & 3,
                     inst->input_index, inst->const_index, rel_addr,
                     &inst->mac_src[2]);

        /* ILU source = source C */
        inst->ilu_src = inst->mac_src[2];

        inst->mac_dst.temp_reg = -1;
        inst->mac_dst.output_reg = NV2A_VSH_OUT_NONE;
        inst->ilu_dst.temp_reg = -1;
        inst->ilu_dst.output_reg = NV2A_VSH_OUT_NONE;

        out_reg = (insn[3] >> 20) & 15;
        if (inst->mac_op != NV2A_VSH_MAC_NOP &&
            inst->mac_op != NV2A_VSH_MAC_ARL &&
            (inst->ilu_op == NV2A_VSH_ILU_NOP || out_reg != 1)) {
            inst->mac_dst.temp_reg = decode_temp_index(out_reg);
            inst->mac_dst.temp_write_mask = (uint8_t)((insn[3] >> 24) & 15);
        }
        if (inst->ilu_op != NV2A_VSH_ILU_NOP) {
            inst->ilu_dst.temp_reg =
                inst->mac_op != NV2A_VSH_MAC_NOP
                    ? 1 : decode_temp_index(out_reg);
            inst->ilu_dst.temp_write_mask = (uint8_t)((insn[3] >> 16) & 15);
        }

        output_mask = (insn[3] >> 12) & 15;
        output_address = (insn[3] >> 3) & 0xFF;
        if (output_mask != 0) {
            NV2AVshDstOperand *dst =
                (insn[3] & 4u) ? &inst->ilu_dst : &inst->mac_dst;
            if (!(insn[3] & 0x800u) || (output_address & 15) >= 13) {
                program->valid = 0;
            } else {
                dst->output_reg = decode_output_mux(output_address);
                dst->output_write_mask = (uint8_t)output_mask;
                if (dst->output_reg == NV2A_VSH_OUT_NONE) {
                    program->valid = 0;
                } else {
                    program->outputs_written |=
                        (uint16_t)(1u << dst->output_reg);
                }
            }
        }

        /* Final instruction flag (bit 0 of word 3) */
        inst->is_final = (insn[3] & 1) ? 1 : 0;

        /* Track input register usage */
        {
            int s;
            for (s = 0; s < 3; s++) {
                if (inst->mac_src[s].reg_type == NV2A_VSH_REG_INPUT)
                    program->inputs_read |= (1u << inst->mac_src[s].reg_index);
            }
            if (inst->ilu_src.reg_type == NV2A_VSH_REG_INPUT)
                program->inputs_read |= (1u << inst->ilu_src.reg_index);
        }

        program->length = i + 1;

        /* Stop at final instruction */
        if (inst->is_final)
            break;
    }
}

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
    int   failed;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    sb->failed = 0;
    if (buf && size > 0)
        buf[0] = '\0';
}

/* ================================================================
 * CPU Position-Bounds Evaluation
 * ================================================================ */

static uint32_t vsh_declaration_storage_size(uint32_t format);

typedef struct NV2AVshVec {
    float v[4];
} NV2AVshVec;

static NV2AVshProgram *vsh_parsed_program(NV2AVshSlot *slot)
{
    if (!slot || !slot->in_use)
        return NULL;
    if (!slot->parsed_program) {
        slot->parsed_program =
            (NV2AVshProgram *)malloc(sizeof(*slot->parsed_program));
        if (!slot->parsed_program)
            return NULL;
        d3d8_vsh_parse(slot->microcode, slot->length,
                       slot->parsed_program);
    }
    return slot->parsed_program->valid ? slot->parsed_program : NULL;
}

static void vsh_read_source_value(const NV2AVshSrcOperand *source,
                                  const NV2AVshVec inputs[16],
                                  const NV2AVshVec temps[13],
                                  const NV2AVshVec outputs[16],
                                  int address,
                                  NV2AVshVec *value)
{
    const NV2AVshVec zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const NV2AVshVec *raw = &zero;
    int index;
    const uint8_t swizzle[4] = {
        source->swizzle.x, source->swizzle.y,
        source->swizzle.z, source->swizzle.w,
    };

    switch (source->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (source->reg_index == 12)
            raw = &outputs[NV2A_VSH_OUT_POS];
        else if (source->reg_index >= 0 && source->reg_index < 13)
            raw = &temps[source->reg_index];
        break;
    case NV2A_VSH_REG_INPUT:
        if (source->reg_index >= 0 && source->reg_index < 16)
            raw = &inputs[source->reg_index];
        break;
    case NV2A_VSH_REG_CONST:
        index = source->reg_index + (source->rel_addr ? address : 0);
        if (index >= 0 && index < NV2A_VS_MAX_CONSTANTS)
            raw = (const NV2AVshVec *)&g_vsh_constants.c[index][0];
        break;
    default:
        break;
    }

    for (int component = 0; component < 4; ++component) {
        float component_value = raw->v[swizzle[component] & 3u];
        value->v[component] = source->negate
            ? -component_value : component_value;
    }
}

static void vsh_write_masked(NV2AVshVec *destination,
                             const NV2AVshVec *value,
                             uint8_t mask)
{
    static const uint8_t component_bits[4] = {8u, 4u, 2u, 1u};
    for (int component = 0; component < 4; ++component) {
        if (mask & component_bits[component])
            destination->v[component] = value->v[component];
    }
}

static void vsh_write_destination(const NV2AVshDstOperand *destination,
                                  const NV2AVshVec *value,
                                  NV2AVshVec temps[13],
                                  NV2AVshVec outputs[16])
{
    if (destination->temp_reg >= 0 && destination->temp_reg < 13 &&
        destination->temp_write_mask) {
        NV2AVshVec *temporary = destination->temp_reg == 12
            ? &outputs[NV2A_VSH_OUT_POS]
            : &temps[destination->temp_reg];
        vsh_write_masked(temporary, value,
                         destination->temp_write_mask);
    }
    if (destination->output_reg != NV2A_VSH_OUT_NONE &&
        destination->output_reg < 16 &&
        destination->output_write_mask) {
        vsh_write_masked(&outputs[destination->output_reg], value,
                         destination->output_write_mask);
    }
}

static NV2AVshVec vsh_mac_value(NV2AVshMacOp operation,
                                const NV2AVshVec *a,
                                const NV2AVshVec *b,
                                const NV2AVshVec *c)
{
    NV2AVshVec result = {{0.0f, 0.0f, 0.0f, 0.0f}};
    float scalar;

    switch (operation) {
    case NV2A_VSH_MAC_MOV:
        return *a;
    case NV2A_VSH_MAC_MUL:
        for (int i = 0; i < 4; ++i) result.v[i] = a->v[i] * b->v[i];
        break;
    case NV2A_VSH_MAC_ADD:
        for (int i = 0; i < 4; ++i) result.v[i] = a->v[i] + c->v[i];
        break;
    case NV2A_VSH_MAC_MAD:
        for (int i = 0; i < 4; ++i)
            result.v[i] = a->v[i] * b->v[i] + c->v[i];
        break;
    case NV2A_VSH_MAC_DP3:
        scalar = a->v[0] * b->v[0] + a->v[1] * b->v[1]
            + a->v[2] * b->v[2];
        for (int i = 0; i < 4; ++i) result.v[i] = scalar;
        break;
    case NV2A_VSH_MAC_DPH:
        scalar = a->v[0] * b->v[0] + a->v[1] * b->v[1]
            + a->v[2] * b->v[2] + b->v[3];
        for (int i = 0; i < 4; ++i) result.v[i] = scalar;
        break;
    case NV2A_VSH_MAC_DP4:
        scalar = a->v[0] * b->v[0] + a->v[1] * b->v[1]
            + a->v[2] * b->v[2] + a->v[3] * b->v[3];
        for (int i = 0; i < 4; ++i) result.v[i] = scalar;
        break;
    case NV2A_VSH_MAC_DST:
        result.v[0] = 1.0f;
        result.v[1] = a->v[1] * b->v[1];
        result.v[2] = a->v[2];
        result.v[3] = b->v[3];
        break;
    case NV2A_VSH_MAC_MIN:
        for (int i = 0; i < 4; ++i) result.v[i] = fminf(a->v[i], b->v[i]);
        break;
    case NV2A_VSH_MAC_MAX:
        for (int i = 0; i < 4; ++i) result.v[i] = fmaxf(a->v[i], b->v[i]);
        break;
    case NV2A_VSH_MAC_SLT:
        for (int i = 0; i < 4; ++i) result.v[i] = a->v[i] < b->v[i] ? 1.0f : 0.0f;
        break;
    case NV2A_VSH_MAC_SGE:
        for (int i = 0; i < 4; ++i) result.v[i] = a->v[i] >= b->v[i] ? 1.0f : 0.0f;
        break;
    default:
        break;
    }
    return result;
}

static NV2AVshVec vsh_ilu_value(NV2AVshIluOp operation,
                                const NV2AVshVec *c)
{
    NV2AVshVec result = {{0.0f, 0.0f, 0.0f, 0.0f}};
    float scalar;

    switch (operation) {
    case NV2A_VSH_ILU_MOV:
        return *c;
    case NV2A_VSH_ILU_RCP:
    case NV2A_VSH_ILU_RCC:
        scalar = 1.0f / c->v[0];
        if (operation == NV2A_VSH_ILU_RCC && isfinite(scalar)) {
            float magnitude = fminf(fmaxf(fabsf(scalar), 5.42101e-20f),
                                    1.84467e19f);
            scalar = copysignf(magnitude, scalar);
        }
        for (int i = 0; i < 4; ++i) result.v[i] = scalar;
        break;
    case NV2A_VSH_ILU_RSQ:
        scalar = 1.0f / sqrtf(fabsf(c->v[0]));
        for (int i = 0; i < 4; ++i) result.v[i] = scalar;
        break;
    case NV2A_VSH_ILU_EXP: {
        float whole = floorf(c->v[0]);
        result.v[0] = exp2f(whole);
        result.v[1] = c->v[0] - whole;
        result.v[2] = exp2f(c->v[0]);
        result.v[3] = 1.0f;
        break;
    }
    case NV2A_VSH_ILU_LOG: {
        float value = fabsf(c->v[0]);
        if (value == 0.0f) {
            result.v[0] = result.v[2] = -INFINITY;
            result.v[1] = result.v[3] = 1.0f;
        } else {
            result.v[0] = floorf(log2f(value));
            result.v[1] = value / exp2f(result.v[0]);
            result.v[2] = log2f(value);
            result.v[3] = 1.0f;
        }
        break;
    }
    case NV2A_VSH_ILU_LIT: {
        float x = fmaxf(c->v[0], 0.0f);
        float y = fmaxf(c->v[1], 0.0f);
        float w = fminf(fmaxf(c->v[3], -127.99609375f), 127.99609375f);
        result.v[0] = result.v[3] = 1.0f;
        result.v[1] = x;
        result.v[2] = x > 0.0f ? exp2f(w * log2f(y)) : 0.0f;
        break;
    }
    default:
        break;
    }
    return result;
}

static BOOL vsh_execute_program(const NV2AVshProgram *program,
                                const NV2AVshVec inputs[16],
                                NV2AVshVec outputs[16])
{
    NV2AVshVec temps[13] = {{{0.0f}}};
    int address = 0;

    if (!program || !program->valid)
        return FALSE;
    for (int output = 0; output < 16; ++output) {
        outputs[output].v[0] = 0.0f;
        outputs[output].v[1] = 0.0f;
        outputs[output].v[2] = 0.0f;
        outputs[output].v[3] = 1.0f;
    }
    for (int instruction = 0; instruction < program->length; ++instruction) {
        const NV2AVshInstruction *slot = &program->insns[instruction];
        NV2AVshVec a, b, c, mac, ilu;
        vsh_read_source_value(&slot->mac_src[0], inputs, temps, outputs,
                              address, &a);
        vsh_read_source_value(&slot->mac_src[1], inputs, temps, outputs,
                              address, &b);
        vsh_read_source_value(&slot->mac_src[2], inputs, temps, outputs,
                              address, &c);
        mac = vsh_mac_value(slot->mac_op, &a, &b, &c);
        ilu = vsh_ilu_value(slot->ilu_op, &c);
        if (slot->mac_op == NV2A_VSH_MAC_ARL)
            address = (int)floorf(a.v[0] + 0.001f);
        else if (slot->mac_op != NV2A_VSH_MAC_NOP)
            vsh_write_destination(&slot->mac_dst, &mac, temps, outputs);
        if (slot->ilu_op != NV2A_VSH_ILU_NOP)
            vsh_write_destination(&slot->ilu_dst, &ilu, temps, outputs);
    }
    return TRUE;
}

static uint32_t vsh_attribute_storage_size(uint32_t format)
{
    return vsh_declaration_storage_size(format);
}

static BOOL vsh_decode_attribute(uint32_t format, const uint8_t *data,
                                 NV2AVshVec *value)
{
    uint32_t type = format & 0xFu;
    uint32_t count = (format >> 4) & 0xFu;
    value->v[0] = value->v[1] = value->v[2] = 0.0f;
    value->v[3] = 1.0f;
    if (!data || count == 0 || count > 4)
        return FALSE;
    if ((format & 0xFFu) == 0x72u) {
        const float *words = (const float *)data;
        value->v[0] = words[0];
        value->v[1] = words[1];
        value->v[3] = words[2];
        return TRUE;
    }
    switch (type) {
    case 2:
        memcpy(value->v, data, count * sizeof(float));
        return TRUE;
    case 0:
        value->v[0] = data[2] * (1.0f / 255.0f);
        value->v[1] = data[1] * (1.0f / 255.0f);
        value->v[2] = data[0] * (1.0f / 255.0f);
        value->v[3] = data[3] * (1.0f / 255.0f);
        return TRUE;
    case 4:
        for (uint32_t i = 0; i < count; ++i)
            value->v[i] = data[i] * (1.0f / 255.0f);
        return TRUE;
    case 1:
    case 5: {
        const int16_t *words = (const int16_t *)data;
        for (uint32_t i = 0; i < count; ++i) {
            value->v[i] = type == 1
                ? fmaxf(-1.0f, words[i] * (1.0f / 32767.0f))
                : (float)words[i];
        }
        return TRUE;
    }
    case 6: {
        int32_t packed;
        int32_t x, y, z;
        memcpy(&packed, data, sizeof(packed));
        x = packed & 0x7FF;
        y = (packed >> 11) & 0x7FF;
        z = (packed >> 22) & 0x3FF;
        if (x & 0x400) x -= 0x800;
        if (y & 0x400) y -= 0x800;
        if (z & 0x200) z -= 0x400;
        value->v[0] = fmaxf(-1.0f, x * (1.0f / 1023.0f));
        value->v[1] = fmaxf(-1.0f, y * (1.0f / 1023.0f));
        value->v[2] = fmaxf(-1.0f, z * (1.0f / 511.0f));
        return TRUE;
    }
    default:
        return FALSE;
    }
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    char sink[1];
    char *target;

    if (sb->failed)
        return;
    remaining = sb->pos < sb->size ? sb->size - sb->pos : 0;
    target = remaining > 0 ? sb->buf + sb->pos : sink;
    va_start(ap, fmt);
    int n = vsnprintf(target, (size_t)remaining, fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->failed = 1;
        return;
    }
    if (n > INT_MAX - sb->pos) {
        sb->failed = 1;
        return;
    }
    /* Track the complete required length even when the caller supplied a
     * smaller buffer. This gives generators snprintf-style sizing semantics
     * instead of silently accepting a valid prefix as a complete shader. */
    sb->pos += n;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index == 12)
            sb_append(sb, "R12"); /* oPos alias */
        else
            sb_append(sb, "R%d", src->reg_index);
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr) {
            sb_append(sb,
                      "((a0 + %d >= 0 && a0 + %d < %d) ? "
                      "vsh_const(a0 + %d) : float4(0,0,0,0))",
                      src->reg_index, src->reg_index,
                      NV2A_VS_MAX_CONSTANTS, src->reg_index);
        } else if (src->reg_index >= 0 &&
                   src->reg_index < NV2A_VS_MAX_CONSTANTS) {
            sb_append(sb, "vsh_const(%d)", src->reg_index);
        } else {
            sb_append(sb, "float4(0,0,0,0)");
        }
        break;
    case NV2A_VSH_REG_ZERO:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit a destination assignment (temp and/or output register write).
 *
 * The NV2A can write to both a temp register and an output register
 * simultaneously from the same operation. We emit two assignments
 * when both are active.
 *
 * @param prefix  The destination: temp register name
 * @param dst     The destination operand
 * @param rhs     The HLSL expression to assign (right-hand side)
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *rhs)
{
    /* Write to temp register if valid */
    if (dst->temp_reg >= 0 && dst->temp_write_mask != 0) {
        if (dst->temp_reg == 12)
            sb_append(sb, "    R12");
        else
            sb_append(sb, "    R%d", dst->temp_reg);
        emit_write_mask(sb, dst->temp_write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->temp_write_mask);
        sb_append(sb, ";\n");
    }

    /* Write to output register if specified */
    if (dst->output_reg != NV2A_VSH_OUT_NONE &&
        dst->output_write_mask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "    %s", name);
            emit_write_mask(sb, dst->output_write_mask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, dst->output_write_mask);
            sb_append(sb, ";\n");
        }
    }
}

/**
 * Emit the HLSL for one MAC operation.
 */
static void emit_slot_source(StrBuf *sb, int instruction, int source,
                             int scalar)
{
    static const char names[] = "ABC";
    sb_append(sb, "vsh%c%d", names[source], instruction);
    if (scalar)
        sb_append(sb, ".x");
}

static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst,
                        int instruction)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_slot_source(&expr, instruction, 0, 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B */
        sb_append(&expr, "(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, " * ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C */
        sb_append(&expr, "(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, " + ");
        emit_slot_source(&expr, instruction, 2, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C */
        sb_append(&expr, "(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, " * ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, " + ");
        emit_slot_source(&expr, instruction, 2, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ".xyz, ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ", ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w) */
        sb_append(&expr, "float4(1.0, ");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ".y * ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ".y, ");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ".z, ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ", ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ", ");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ", ");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_slot_source(&expr, instruction, 1, 0);
        sb_append(&expr, ", ");
        emit_slot_source(&expr, instruction, 0, 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* a0 = floor(A.x) - special: writes address register, not a float reg */
        sb_append(sb, "    a0 = (int)floor(");
        emit_slot_source(sb, instruction, 0, 0);
        sb_append(sb, ".x + 0.001);\n");
        return; /* No destination register write */

    default:
        return;
    }

    emit_dest_assign(sb, &inst->mac_dst, expr_buf);
}

/**
 * Emit the HLSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst,
                        int instruction)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_slot_source(&expr, instruction, 2, 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_slot_source(&expr, instruction, 2, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        sb_append(&expr, "vsh_rcc(");
        emit_slot_source(&expr, instruction, 2, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_slot_source(&expr, instruction, 2, 1);
        sb_append(&expr, ")).xxxx");
        break;

    case NV2A_VSH_ILU_EXP:
        sb_append(&expr, "vsh_exp(");
        emit_slot_source(&expr, instruction, 2, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_LOG: {
        sb_append(&expr, "vsh_log(");
        emit_slot_source(&expr, instruction, 2, 1);
        sb_append(&expr, ")");
        break;
    }

    case NV2A_VSH_ILU_LIT: {
        sb_append(&expr, "vsh_lit(");
        emit_slot_source(&expr, instruction, 2, 0);
        sb_append(&expr, ")");
        break;
    }

    default:
        return;
    }

    emit_dest_assign(sb, &inst->ilu_dst, expr_buf);
}

/**
 * Map an NV2A input register index to the canonical shader input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
static const char *input_semantic_name(int reg_index)
{
    /* All inputs use the generic TEXCOORD semantic with unique indices
     * to avoid mismatches. The input layout will map them correctly. */
    (void)reg_index;
    return "ATTR";
}

/* Map an NV2A vertex attribute format (type in bits[3:0], component count in
 * bits[7:4]) to the HLSL input element type and the expression that unpacks
 * input.v<attr> to the CPU-decoder-equivalent float4. The Plume input layout
 * binds a DXGI format matching this: F->R32*_FLOAT, UB_D3D->B8G8R8A8_UNORM,
 * UB_OGL->R8G8B8A8_UNORM, S1->R16*_SNORM (all auto-convert to float4), S32K->
 * R16*_SINT (int, cast here), CMP->R32_SINT (unpacked here). */
static void vsh_raw_input(uint32_t format, int attr,
                          const char **decl, char *unpack, int usz)
{
    uint32_t type = format & 0xFu;
    uint32_t count = (format >> 4) & 0xFu;
    switch (type) {
    case 2: /* F, including Xbox FLOAT2H (x, y, w -> x, y, 0, w). */
        if ((format & 0xFFu) == 0x72u) {
            *decl = "float3";
            snprintf(unpack, usz,
                     "float4(input.v%d.xy, 0.0, input.v%d.z)",
                     attr, attr);
        } else {
            *decl = "float4";
            snprintf(unpack, usz, "input.v%d", attr);
        }
        break;
    case 4: /* Packed normalized bytes. */
        *decl = "float4";
        if (count == 3)
            snprintf(unpack, usz, "float4(input.v%d.xyz, 1.0)", attr);
        else
            snprintf(unpack, usz, "input.v%d", attr);
        break;
    case 5: /* S32K: raw int16 values; pad missing components with 0,0,0,1. */
        *decl = "int4";
        if (count >= 4)
            snprintf(unpack, usz, "float4(input.v%d)", attr);
        else if (count == 3)
            snprintf(unpack, usz, "float4(float3(input.v%d.xyz), 1.0)", attr);
        else if (count == 2)
            snprintf(unpack, usz, "float4(float2(input.v%d.xy), 0.0, 1.0)", attr);
        else
            snprintf(unpack, usz, "float4(float(input.v%d.x), 0.0, 0.0, 1.0)", attr);
        break;
    case 6: /* CMP: one dword packing 11/11/10 signed components. */
        *decl = "int";
        snprintf(unpack, usz, "vsh_unpack_cmp(input.v%d)", attr);
        break;
    default: /* F / UB_D3D / UB_OGL / S1: input assembler yields float4. */
        *decl = "float4";
        if (type == 1 && count == 3)
            /* S1 x3: no 16-bit 3-component DXGI format exists, so the layout
             * binds R16G16B16A16_SNORM and over-reads a 4th short into .w;
             * use .xyz and restore the decoder's w=1 default. */
            snprintf(unpack, usz, "float4(input.v%d.xyz, 1.0)", attr);
        else
            snprintf(unpack, usz, "input.v%d", attr);
        break;
    }
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            const uint32_t *vertex_format,
                            char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs;
    int need_cmp = 0;

    if (!program || !program->valid || bufsize < 0 ||
        (bufsize > 0 && !buf))
        return -1;
    inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* One frame buffer holds every draw's 192-float4 constant block. */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "struct NdcScaleData { float halfW; float halfH; float homogeneous; "
        "float viewportZOffset; float viewportZScale; "
        "uint vsConstantBase; float uiScaleX; float uiOffsetX; };\n"
        "#ifdef XGPU_SPIRV\n"
        "[[vk::push_constant]] ConstantBuffer<NdcScaleData> ndcScale;\n"
        "#else\n"
        "ConstantBuffer<NdcScaleData> ndcScale : register(b0);\n"
        "#endif\n"
        "\n"
        "StructuredBuffer<float4> xgpuVSConstants : register(t4);\n"
        "float4 vsh_const(int index) {\n"
        "    return xgpuVSConstants[ndcScale.vsConstantBase + index];\n"
        "}\n"
        "\n"
        "float vsh_rcc(float x) {\n"
        "    float value = 1.0 / x;\n"
        "    if (isfinite(value)) {\n"
        "        float magnitude = clamp(abs(value), 5.42101e-20, 1.84467e19);\n"
        "        value = (asuint(value) & 0x80000000u) != 0u\n"
        "            ? -magnitude : magnitude;\n"
        "    }\n"
        "    return value;\n"
        "}\n"
        "float4 vsh_exp(float x) {\n"
        "    float whole = floor(x);\n"
        "    return float4(exp2(whole), x - whole, exp2(x), 1.0);\n"
        "}\n"
        "float4 vsh_log(float x) {\n"
        "    float value = abs(x);\n"
        "    if (value == 0.0) {\n"
        "        float negInf = asfloat(0xFF800000u);\n"
        "        return float4(negInf, 1.0, negInf, 1.0);\n"
        "    }\n"
        "    float whole = floor(log2(value));\n"
        "    return float4(whole, value / exp2(whole), log2(value), 1.0);\n"
        "}\n"
        "float4 vsh_lit(float4 value) {\n"
        "    float x = max(value.x, 0.0);\n"
        "    float y = max(value.y, 0.0);\n"
        "    float w = clamp(value.w, -127.99609375, 127.99609375);\n"
        "    return float4(1.0, x,\n"
        "                  x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0);\n"
        "}\n"
        "\n");

    /* CMP unpack helper (emitted only when a CMP attribute is read). */
    if (vertex_format) {
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
            if ((inputs & (1u << i)) && (vertex_format[i] & 0xFu) == 6u)
                need_cmp = 1;
    }
    if (need_cmp) {
        sb_append(&sb,
            "float4 vsh_unpack_cmp(int p) {\n"
            "    int x = p & 0x7FF; if (x & 0x400) x -= 0x800;\n"
            "    int y = (p >> 11) & 0x7FF; if (y & 0x400) y -= 0x800;\n"
            "    int z = (p >> 22) & 0x3FF; if (z & 0x200) z -= 0x400;\n"
            "    return float4(max(-1.0, x / 1023.0), max(-1.0, y / 1023.0),\n"
            "                  max(-1.0, z / 511.0), 1.0);\n"
            "}\n\n");
    }

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            if (vertex_format) {
                const char *decl = "float4";
                char unpack[64];
                vsh_raw_input(vertex_format[i], i, &decl, unpack, sizeof(unpack));
                sb_append(&sb, "    %s v%d : ATTR%d;\n", decl, i, i);
            } else {
                sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
            }
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        /* `precise` forbids reassociation and mad-contraction on everything
         * feeding position. We emit one HLSL variant per vertex layout, and
         * the compiler otherwise schedules each differently -- enough to move
         * the result by an ULP. Titles that shade a second pass with
         * D3DCMP_EQUAL (MM3's buildings) then lose every fragment whose depth
         * does not match the depth pass bit-exactly, which reads on screen as
         * the geometry being riddled with holes. Measured: mm3roam3.rdc EID
         * 603 vs 614, same primitive, bit-identical input bytes, depths
         * 0.9950074553489685 vs 0.9950075149536133. */
        "    precise float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            if (vertex_format) {
                const char *decl = "float4";
                char unpack[64];
                vsh_raw_input(vertex_format[i], i, &decl, unpack, sizeof(unpack));
                sb_append(&sb, "    float4 v%d = %s;\n", i, unpack);
                continue;
            }
            sb_append(&sb, "    float4 v%d = input.v%d;\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables */
    sb_append(&sb,
        "    /* Output registers (initialized to zero) */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        sb_append(&sb, "\n    /* Instruction %d */\n", i);
        /* Both NV2A execution units read their sources before either unit
         * writes. Snapshot A/B/C so generated sequential HLSL preserves that
         * parallel-instruction behavior even when a destination aliases C. */
        sb_append(&sb, "    float4 vshA%d = ", i);
        emit_source(&sb, &inst->mac_src[0], 0);
        sb_append(&sb, ";\n    float4 vshB%d = ", i);
        emit_source(&sb, &inst->mac_src[1], 0);
        sb_append(&sb, ";\n    float4 vshC%d = ", i);
        emit_source(&sb, &inst->mac_src[2], 0);
        sb_append(&sb, ";\n");

        /* MAC operation */
        if (inst->mac_op != NV2A_VSH_MAC_NOP)
            emit_mac_op(&sb, inst, i);

        /* ILU operation (executes in parallel with MAC on hardware;
         * in HLSL they are sequential but semantically equivalent
         * because ILU reads source C, not MAC destinations) */
        if (inst->ilu_op != NV2A_VSH_ILU_NOP)
            emit_ilu_op(&sb, inst, i);
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Populate output structure */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    float clipWMag = clamp(abs(oPos.w), 5.42101e-20, 1.84467e19);\n"
        "    float clipW = (asuint(oPos.w) & 0x80000000u) != 0u\n"
        "        ? -clipWMag : clipWMag;\n"
        "    clipW = isnan(oPos.w) ? 1.0 : clipW;\n"
        "    float uiX = oPos.x * ndcScale.uiScaleX + ndcScale.uiOffsetX;\n"
        "    o.oPos.x = (uiX / ndcScale.halfW - 1.0) * clipW;\n"
        "    o.oPos.y = (1.0 - oPos.y / ndcScale.halfH) * clipW;\n"
        "    float viewportZ = ndcScale.viewportZScale != 0.0\n"
        "        ? (oPos.z - ndcScale.viewportZOffset) / ndcScale.viewportZScale\n"
        "        : oPos.z;\n"
        "    o.oPos.z = viewportZ * clipW;\n"
        "    o.oPos.w = clipW;\n"
        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.failed ? -1 : sb.pos;
}

int d3d8_vsh_generate_compute_hlsl(const NV2AVshProgram *program,
                                   char *buf, int bufsize)
{
    StrBuf sb;
    int i;

    if (!program || !program->valid || bufsize < 0 ||
        (bufsize > 0 && !buf))
        return -1;

    sb_init(&sb, buf, bufsize);
    sb_append(&sb,
        "/* Auto-generated NV2A vertex-program differential compute shader */\n"
        "StructuredBuffer<float4> vshInputs : register(t0);\n"
        "RWStructuredBuffer<float4> vshOutputs : register(u1);\n"
        "cbuffer VSH_Constants : register(b9) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "float4 vsh_const(int index) { return c[index]; }\n"
        "\n"
        "float vsh_rcc(float x) {\n"
        "    float value = 1.0 / x;\n"
        "    if (isfinite(value)) {\n"
        "        float magnitude = clamp(abs(value), 5.42101e-20, 1.84467e19);\n"
        "        value = (asuint(value) & 0x80000000u) != 0u\n"
        "            ? -magnitude : magnitude;\n"
        "    }\n"
        "    return value;\n"
        "}\n"
        "float4 vsh_exp(float x) {\n"
        "    float whole = floor(x);\n"
        "    return float4(exp2(whole), x - whole, exp2(x), 1.0);\n"
        "}\n"
        "float4 vsh_log(float x) {\n"
        "    float value = abs(x);\n"
        "    if (value == 0.0) {\n"
        "        float negInf = asfloat(0xFF800000u);\n"
        "        return float4(negInf, 1.0, negInf, 1.0);\n"
        "    }\n"
        "    float whole = floor(log2(value));\n"
        "    return float4(whole, value / exp2(whole), log2(value), 1.0);\n"
        "}\n"
        "float4 vsh_lit(float4 value) {\n"
        "    float x = max(value.x, 0.0);\n"
        "    float y = max(value.y, 0.0);\n"
        "    float w = clamp(value.w, -127.99609375, 127.99609375);\n"
        "    return float4(1.0, x,\n"
        "                  x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0);\n"
        "}\n"
        "\n"
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 dispatchId : SV_DispatchThreadID) {\n"
        "    if (any(dispatchId != uint3(0, 0, 0))) return;\n",
        NV2A_VS_MAX_CONSTANTS);

    for (i = 0; i <= 12; i++)
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    sb_append(&sb, "    int a0 = 0;\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (program->inputs_read & (1u << i))
            sb_append(&sb, "    float4 v%d = vshInputs[%d];\n", i, i);
    }
    sb_append(&sb,
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "    #define R12 oPos\n");

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];
        sb_append(&sb, "    float4 vshA%d = ", i);
        emit_source(&sb, &inst->mac_src[0], 0);
        sb_append(&sb, ";\n    float4 vshB%d = ", i);
        emit_source(&sb, &inst->mac_src[1], 0);
        sb_append(&sb, ";\n    float4 vshC%d = ", i);
        emit_source(&sb, &inst->mac_src[2], 0);
        sb_append(&sb, ";\n");
        if (inst->mac_op != NV2A_VSH_MAC_NOP)
            emit_mac_op(&sb, inst, i);
        if (inst->ilu_op != NV2A_VSH_ILU_NOP)
            emit_ilu_op(&sb, inst, i);
    }

    sb_append(&sb,
        "    #undef R12\n"
        "    vshOutputs[0] = oPos;\n"
        "    vshOutputs[1] = float4(0,0,0,1);\n"
        "    vshOutputs[2] = float4(0,0,0,1);\n"
        "    vshOutputs[3] = oD0;\n"
        "    vshOutputs[4] = oD1;\n"
        "    vshOutputs[5] = oFog;\n"
        "    vshOutputs[6] = oPts;\n"
        "    vshOutputs[7] = oB0;\n"
        "    vshOutputs[8] = oB1;\n"
        "    vshOutputs[9] = oT0;\n"
        "    vshOutputs[10] = oT1;\n"
        "    vshOutputs[11] = oT2;\n"
        "    vshOutputs[12] = oT3;\n"
        "    vshOutputs[13] = float4(0,0,0,1);\n"
        "    vshOutputs[14] = float4(0,0,0,1);\n"
        "    vshOutputs[15] = float4(0,0,0,1);\n"
        "}\n");

    return sb.failed ? -1 : sb.pos;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

/* ================================================================
 * Public API Implementation
 * ================================================================ */

static uint32_t vsh_declaration_storage_size(uint32_t format)
{
    switch (format & 0xFFu) {
    case 0x12u: return 4;  /* FLOAT1 */
    case 0x22u: return 8;  /* FLOAT2 */
    case 0x32u: return 12; /* FLOAT3 */
    case 0x42u: return 16; /* FLOAT4 */
    case 0x40u: return 4;  /* D3DCOLOR */
    case 0x25u: return 4;  /* SHORT2 */
    case 0x45u: return 8;  /* SHORT4 */
    case 0x11u: return 2;  /* NORMSHORT1 */
    case 0x21u: return 4;  /* NORMSHORT2 */
    case 0x31u: return 6;  /* NORMSHORT3 */
    case 0x41u: return 8;  /* NORMSHORT4 */
    case 0x16u: return 4;  /* NORMPACKED3 */
    case 0x15u: return 2;  /* SHORT1 */
    case 0x35u: return 6;  /* SHORT3 */
    case 0x14u: return 1;  /* PBYTE1 */
    case 0x24u: return 2;  /* PBYTE2 */
    case 0x34u: return 3;  /* PBYTE3 */
    case 0x44u: return 4;  /* PBYTE4 */
    case 0x72u: return 12; /* FLOAT2H: x, y, w */
    case 0x02u: return 0;  /* NONE */
    default: return UINT32_MAX;
    }
}

BOOL d3d8_vsh_parse_declaration(const DWORD *tokens, size_t max_dwords,
                                NV2AVshDeclaration *declaration)
{
    enum {
        VSD_TOKEN_NOP = 0,
        VSD_TOKEN_STREAM = 1,
        VSD_TOKEN_STREAMDATA = 2,
        VSD_TOKEN_TESSELLATOR = 3,
        VSD_TOKEN_CONSTMEM = 4,
        VSD_TOKEN_EXT = 5,
        VSD_TOKEN_END = 7,
    };
    uint32_t stream_offset[16] = {0};
    uint32_t current_stream = 0;
    size_t cursor = 0;

    if (!tokens || !max_dwords || !declaration)
        return FALSE;
    memset(declaration, 0, sizeof(*declaration));
    memset(declaration->format, 0x02, sizeof(declaration->format));

    while (cursor < max_dwords) {
        uint32_t token = tokens[cursor++];
        uint32_t type = token >> 29;
        switch (type) {
        case VSD_TOKEN_NOP:
            if (token != 0)
                return FALSE;
            break;
        case VSD_TOKEN_STREAM:
            /* Tessellator streams require generated attributes, which the
             * raw host input-layout path cannot represent yet. */
            if (token & 0x10000000u)
                return FALSE;
            current_stream = token & 0xFu;
            break;
        case VSD_TOKEN_STREAMDATA:
            if (token & 0x10000000u) {
                uint32_t count = (token >> 16) & 0xFu;
                uint32_t bytes =
                    (token & 0x08000000u) ? count : count * 4u;
                if (stream_offset[current_stream] > UINT16_MAX - bytes)
                    return FALSE;
                stream_offset[current_stream] += bytes;
            } else {
                uint32_t reg = token & 0x1Fu;
                uint32_t format = (token >> 16) & 0xFFu;
                uint32_t bytes = vsh_declaration_storage_size(format);
                if (reg >= NV2A_VS_MAX_INPUTS || bytes == UINT32_MAX)
                    return FALSE;
                if (stream_offset[current_stream] > UINT16_MAX ||
                    bytes > UINT16_MAX - stream_offset[current_stream])
                    return FALSE;
                declaration->stream[reg] = (uint8_t)current_stream;
                declaration->format[reg] = (uint8_t)format;
                declaration->offset[reg] =
                    (uint16_t)stream_offset[current_stream];
                if (format == 0x02u)
                    declaration->attributes_present &=
                        (uint16_t)~(1u << reg);
                else
                    declaration->attributes_present |=
                        (uint16_t)(1u << reg);
                stream_offset[current_stream] += bytes;
            }
            break;
        case VSD_TOKEN_TESSELLATOR:
            return FALSE;
        case VSD_TOKEN_CONSTMEM: {
            uint32_t count = (token >> 25) & 0xFu;
            size_t payload = (size_t)count * 4u;
            if (!count || payload > max_dwords - cursor)
                return FALSE;
            cursor += payload;
            break;
        }
        case VSD_TOKEN_EXT: {
            uint32_t count = (token >> 24) & 0x1Fu;
            if ((size_t)count > max_dwords - cursor)
                return FALSE;
            cursor += count;
            break;
        }
        case VSD_TOKEN_END:
            if (token != 0xFFFFFFFFu)
                return FALSE;
            declaration->valid = 1;
            return TRUE;
        default:
            return FALSE;
        }
    }
    return FALSE;
}

HRESULT d3d8_vsh_init(void)
{
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; ++i)
        free(g_vsh_slots[i].parsed_program);
    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_registered, 0, sizeof(g_vsh_registered));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    memset(g_vsh_vertex_data, 0, sizeof(g_vsh_vertex_data));
    for (int i = 0; i < NV2A_VS_MAX_INPUTS; ++i)
        g_vsh_vertex_data[i][3] = 1.0f;
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; ++i)
        free(g_vsh_slots[i].parsed_program);
    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_registered, 0, sizeof(g_vsh_registered));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    memset(g_vsh_vertex_data, 0, sizeof(g_vsh_vertex_data));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                const NV2AVshDeclaration *declaration,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation) */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    memset(&g_vsh_slots[slot].declaration, 0,
           sizeof(g_vsh_slots[slot].declaration));
    if (declaration && declaration->valid)
        g_vsh_slots[slot].declaration = *declaration;
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].parsed_program = NULL;
    g_vsh_slots[slot].in_use = 1;
    g_vsh_registered[slot] = FALSE;
    g_vsh_slot_count++;

    /* Host shader handles use a narrow range so high FVF texture-coordinate
     * size bits cannot be mistaken for programmable shaders. */
    *out_handle = (DWORD)(slot + NV2A_VS_HANDLE_BASE);

    fprintf(stderr, "D3D8 VSH: Created shader handle 0x%lX (%d instructions)\n",
            *out_handle, num_insns);

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - NV2A_VS_HANDLE_BASE);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        free(g_vsh_slots[slot].parsed_program);
        g_vsh_slots[slot].parsed_program = NULL;
        g_vsh_slots[slot].in_use = 0;
        g_vsh_registered[slot] = FALSE;
        g_vsh_slot_count--;
    }

    return S_OK;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;
    size_t byte_count;

    if (!data || start_reg < 0 || start_reg >= NV2A_VS_MAX_CONSTANTS ||
        count <= 0)
        return;

    if (count > NV2A_VS_MAX_CONSTANTS - start_reg)
        count = NV2A_VS_MAX_CONSTANTS - start_reg;
    end_reg = start_reg + count;
    byte_count = (size_t)(end_reg - start_reg) *
                 sizeof(g_vsh_constants.c[0]);
    if (memcmp(&g_vsh_constants.c[start_reg][0], data, byte_count) == 0)
        return;

    memcpy(&g_vsh_constants.c[start_reg][0], data, byte_count);
    g_vsh_constants_dirty = TRUE;
}

void d3d8_vsh_set_vertex_data4f(uint32_t reg, const float *value)
{
    if (reg >= NV2A_VS_MAX_INPUTS || !value)
        return;
    memcpy(g_vsh_vertex_data[reg], value,
           sizeof(g_vsh_vertex_data[reg]));
}

BOOL d3d8_vsh_calculate_position_bounds(DWORD handle,
                                         const void *vertices,
                                         uint32_t vertex_count,
                                         uint32_t stride,
                                         float *minimum_x,
                                         float *maximum_x)
{
    NV2AVshSlot *slot;
    NV2AVshProgram *program;
    const uint8_t *vertex_bytes = (const uint8_t *)vertices;
    float minimum = 0.0f;
    float maximum = 0.0f;

    if (!d3d8_vsh_is_programmable(handle) || !vertices ||
        !vertex_count || !stride || !minimum_x || !maximum_x)
        return FALSE;
    slot = &g_vsh_slots[handle - NV2A_VS_HANDLE_BASE];
    if (!slot->in_use || !slot->declaration.valid)
        return FALSE;
    program = vsh_parsed_program(slot);
    if (!program)
        return FALSE;

    for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
        NV2AVshVec inputs[16];
        NV2AVshVec outputs[16];
        for (uint32_t attr = 0; attr < 16; ++attr) {
            memcpy(inputs[attr].v, g_vsh_vertex_data[attr],
                   sizeof(inputs[attr].v));
            if (!(program->inputs_read & (1u << attr)) ||
                !(slot->declaration.attributes_present & (1u << attr)))
                continue;
            if (slot->declaration.stream[attr] != 0)
                return FALSE;
            uint32_t size = vsh_attribute_storage_size(
                slot->declaration.format[attr]);
            uint32_t offset = slot->declaration.offset[attr];
            if (size == UINT32_MAX || size > stride || offset > stride - size ||
                !vsh_decode_attribute(
                    slot->declaration.format[attr],
                    vertex_bytes + (size_t)vertex * stride + offset,
                    &inputs[attr]))
                return FALSE;
        }
        if (!vsh_execute_program(program, inputs, outputs) ||
            !isfinite(outputs[NV2A_VSH_OUT_POS].v[0]))
            return FALSE;
        if (vertex == 0) {
            minimum = maximum = outputs[NV2A_VSH_OUT_POS].v[0];
        } else {
            minimum = fminf(minimum, outputs[NV2A_VSH_OUT_POS].v[0]);
            maximum = fmaxf(maximum, outputs[NV2A_VSH_OUT_POS].v[0]);
        }
    }
    *minimum_x = minimum;
    *maximum_x = maximum;
    return TRUE;
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return handle >= NV2A_VS_HANDLE_BASE &&
           handle < NV2A_VS_HANDLE_LIMIT ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    int slot;
    NV2AVshSlot *vsh;
    NV2AVshProgram *program;
    XgpuPlumeVertexDeclaration plume_declaration;
    const XgpuPlumeVertexDeclaration *plume_declaration_ptr = NULL;
    uint32_t vertex_format[NV2A_VS_MAX_INPUTS];
    char *source = NULL;
    int length;

    if (!d3d8_vsh_is_programmable(handle)) {
        xgpu_plume_set_active_vertex_shader(0);
        return FALSE;
    }

    /* Resolve handle to shader slot */
    slot = (int)(handle - NV2A_VS_HANDLE_BASE);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    if (!g_vsh_registered[slot]) {
        int i;
        program = vsh_parsed_program(vsh);
        if (!program)
            return FALSE;
        if (vsh->declaration.valid) {
            memset(&plume_declaration, 0, sizeof(plume_declaration));
            plume_declaration.attributes_present =
                vsh->declaration.attributes_present;
            for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
                vertex_format[i] = vsh->declaration.format[i];
                plume_declaration.stream[i] =
                    vsh->declaration.stream[i];
                plume_declaration.format[i] =
                    vsh->declaration.format[i];
                plume_declaration.offset[i] =
                    vsh->declaration.offset[i];
            }
            plume_declaration_ptr = &plume_declaration;
        }
        length = d3d8_vsh_generate_hlsl(
            program, vsh->declaration.valid ? vertex_format : NULL, NULL, 0);
        if (length <= 0)
            return FALSE;
        source = (char *)malloc((size_t)length + 1);
        if (!source)
            return FALSE;
        if (d3d8_vsh_generate_hlsl(
                program, vsh->declaration.valid ? vertex_format : NULL,
                source, length + 1) != length) {
            free(source);
            return FALSE;
        }
        if (!xgpu_plume_register_vertex_shader(handle, source,
                                                program->inputs_read,
                                                program->outputs_written,
                                                plume_declaration_ptr)) {
            free(source);
            return FALSE;
        }
        free(source);
        source = NULL;
        g_vsh_registered[slot] = TRUE;
    }
    if (g_vsh_constants_dirty) {
        xgpu_plume_set_vertex_shader_constants(
            (const float *)&g_vsh_constants, NV2A_VS_MAX_CONSTANTS);
        g_vsh_constants_dirty = FALSE;
    }
    xgpu_plume_set_active_vertex_shader(handle);
    return TRUE;
}
