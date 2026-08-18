/*
 * xps_translate.cpp — see xps_translate.h.
 *
 * Scope: the DX8/Xbox ps.1.1 dialect MM3 actually emits (verified by dumping the
 * fragment table in the XBE at 0x3ab530 + the sprintf templates at ~0x3aba00):
 *   ops : tex, mov, mad, mul, add, sub, lrp, cnd, dp3, xfc (final combiner)
 *   result mods : _x2 _x4 _x8 _d2 _d4 _sat
 *   source mods : "1 - x" (complement), "-x" (negate), _sat, _bias, _bx2
 *   swizzles    : .rgb .a .r .g .b (+ replication), co-issue prefix '+'
 *   specials    : zero, one, v0/v1 (iterated colors), prod (xfc only)
 *
 * The emitter is deliberately literal: each instruction becomes one HLSL
 * statement writing only its destination write-mask, so RGB/alpha co-issue pairs
 * (the '+' lines) fall out naturally as two statements touching disjoint lanes.
 * dxc handles register allocation and folding.
 */
#include "xps_translate.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* ── small string helpers ───────────────────────────────────────────────── */

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) { out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}

/* ── operand model ──────────────────────────────────────────────────────── */

struct Src {
    std::string reg;      /* r0,r1,t0..t3,c0..c7,v0,v1,zero,one,prod */
    std::string swizzle;  /* "", "rgb", "a", "b", ... (no leading dot)  */
    bool sat = false, bias = false, bx2 = false, neg = false, comp = false;
};

/* Per-shader inline `def cN, x,y,z,w` literals (empty = not defined). Reset at
 * the top of each xrecomp_xps_to_hlsl call; shader creation is single-threaded. */
static std::string s_defConst[8];

/* Base HLSL expression for a register (no swizzle/mods yet). r0/r1 (temp),
 * t0-3 (texture), v0/v1 (iterated colors), prod/sum (combiner pseudo-regs) and
 * discard are all backed by mutable locals so they can be read and written. */
std::string reg_base(const std::string &r)
{
    if (r == "r0" || r == "r1" ||
        r == "t0" || r == "t1" || r == "t2" || r == "t3" ||
        r == "v0" || r == "v1" || r == "prod" || r == "sum")
        return r;
    if (r == "discard") return "discardReg";
    if (r.size() == 2 && r[0] == 'c' && r[1] >= '0' && r[1] <= '9') {
        int n = r[1] - '0';
        if (n < 8 && !s_defConst[n].empty()) return s_defConst[n];
        return std::string("c[") + r[1] + "]";
    }
    if (r == "zero") return "0.0";
    if (r == "one")  return "1.0";
    return "0.0";
}

/* Local variable name for a destination register (writes to zero/one/discard
 * go to a throwaway). */
std::string dest_name(const std::string &r)
{
    if (r == "discard" || r == "zero" || r == "one") return "discardReg";
    return r;
}

bool is_scalar_literal(const std::string &r) { return r == "zero" || r == "one"; }

/* Parse a single source operand token into a Src. */
Src parse_src(std::string tok)
{
    Src s;
    tok = trim(tok);

    /* leading "1 - x" complement (with or without spaces) */
    if (!tok.empty() && tok[0] == '1') {
        size_t k = 1;
        while (k < tok.size() && std::isspace((unsigned char)tok[k])) k++;
        if (k < tok.size() && tok[k] == '-') {
            s.comp = true;
            k++;
            while (k < tok.size() && std::isspace((unsigned char)tok[k])) k++;
            tok = tok.substr(k);
        } else if (trim(tok) == "1") {
            s.reg = "one";
            return s;
        }
    }
    /* leading unary negate */
    if (!tok.empty() && tok[0] == '-') { s.neg = true; tok = trim(tok.substr(1)); }

    /* split off swizzle */
    size_t dot = tok.find('.');
    std::string head = (dot == std::string::npos) ? tok : tok.substr(0, dot);
    if (dot != std::string::npos) s.swizzle = tok.substr(dot + 1);

    /* head = reg[_mod][_mod]... */
    std::vector<std::string> parts = split(head, '_');
    s.reg = parts.empty() ? head : parts[0];
    for (size_t i = 1; i < parts.size(); i++) {
        if (parts[i] == "sat")  s.sat = true;
        else if (parts[i] == "bias") s.bias = true;
        else if (parts[i] == "bx2")  s.bx2 = true;
        /* _x2/_pp on a source are rare in MM3; ignore quietly */
    }
    return s;
}

/* Render a source at a given lane width (1, 3, or 4). */
std::string emit_src(const Src &s, int width)
{
    std::string expr;
    if (is_scalar_literal(s.reg)) {
        expr = reg_base(s.reg);   /* scalar; promotes in vector arithmetic */
    } else {
        std::string base = reg_base(s.reg);
        std::string sw = s.swizzle;
        if (sw.empty()) {
            sw = (width == 1) ? "a" : (width == 3 ? "rgb" : "");
        } else if ((int)sw.size() == 1 && width > 1) {
            sw = std::string((size_t)width, sw[0]);   /* replicate, e.g. .a -> .aaa */
        } else if (width == 1 && sw.size() > 1) {
            sw = sw.substr(0, 1);
        } else if (width >= 2 && (int)sw.size() != width) {
            /* coerce a multi-char swizzle to exactly `width` lanes (e.g. an
             * .rgb source feeding an .rgba op -> .rgba) so vector widths match. */
            static const char *canon = "rgba";
            if ((int)sw.size() > width) sw = sw.substr(0, (size_t)width);
            else while ((int)sw.size() < width) sw += canon[sw.size()];
        }
        expr = base + (sw.empty() ? "" : ("." + sw));
    }
    if (s.sat)  expr = "saturate(" + expr + ")";
    if (s.bias) expr = "(" + expr + " - 0.5)";
    if (s.bx2)  expr = "(" + expr + " * 2.0 - 1.0)";
    if (s.neg)  expr = "(-" + expr + ")";
    if (s.comp) expr = "(1.0 - " + expr + ")";
    return expr;
}

struct Dest {
    std::string reg;   /* r0 / r1 (or texture reg for `tex`) */
    std::string mask;  /* rgba / rgb / a / ... (no dot) */
    int width() const { return mask.empty() ? 4 : (int)mask.size(); }
};

Dest parse_dest(const std::string &tok)
{
    Dest d;
    std::string t = trim(tok);
    size_t dot = t.find('.');
    d.reg  = (dot == std::string::npos) ? t : t.substr(0, dot);
    d.mask = (dot == std::string::npos) ? "rgba" : t.substr(dot + 1);
    /* strip any dest modifier suffix accidentally captured (e.g. r0_sat) */
    size_t us = d.reg.find('_');
    if (us != std::string::npos) d.reg = d.reg.substr(0, us);
    return d;
}

/* Apply result modifiers (opcode suffixes) to a computed value. */
std::string apply_result_mods(std::string val, const std::vector<std::string> &mods)
{
    val = "(" + val + ")";
    for (const std::string &m : mods) {
        if (m == "x2") val += " * 2.0";
        else if (m == "x4") val += " * 4.0";
        else if (m == "x8") val += " * 8.0";
        else if (m == "d2") val += " * 0.5";
        else if (m == "d4") val += " * 0.25";
    }
    for (const std::string &m : mods)
        if (m == "sat") { val = "saturate(" + val + ")"; break; }
    return val;
}

/* Emit "  <dest>.<mask> = <result-modified val>;" for one destination. */
void emit_masked(std::ostringstream &body, const Dest &d, const std::string &val,
                 const std::vector<std::string> &mods)
{
    body << "  " << dest_name(d.reg) << "." << d.mask << " = "
         << apply_result_mods(val, mods) << ";\n";
}

} // namespace

XpsTranslateResult xrecomp_xps_to_hlsl(const char *src)
{
    XpsTranslateResult res;
    res.ok = true;

    std::ostringstream body;
    std::ostringstream warn;

    std::string text = src ? src : "";
    std::vector<std::string> lines = split(text, '\n');

    /* Pre-scan `def cN, x, y, z, w` inline constants (used before their
     * definition line is reached in the body loop below). */
    for (int i = 0; i < 8; i++) s_defConst[i].clear();
    for (std::string raw : lines) {
        size_t sc = raw.find(';');
        if (sc != std::string::npos) raw = raw.substr(0, sc);
        std::string ln = trim(raw);
        if (ln.rfind("def", 0) != 0) continue;
        std::vector<std::string> parts;
        for (const std::string &o : split(trim(ln.substr(3)), ','))
            parts.push_back(trim(o));
        if (parts.size() >= 5 && parts[0].size() == 2 && parts[0][0] == 'c') {
            int n = parts[0][1] - '0';
            if (n >= 0 && n < 8)
                s_defConst[n] = "float4(" + parts[1] + ", " + parts[2] + ", " +
                                parts[3] + ", " + parts[4] + ")";
        }
    }

    for (std::string raw : lines) {
        /* strip inline comment and normalise whitespace */
        size_t semi = raw.find(';');
        if (semi != std::string::npos) raw = raw.substr(0, semi);
        std::string line = trim(raw);
        if (line.empty()) continue;

        /* version decl / phase markers */
        if (line.rfind("xps.", 0) == 0 || line.rfind("ps.", 0) == 0 ||
            line == "phase" || line == "nop")
            continue;

        bool coissue = false;
        if (line[0] == '+') { coissue = true; line = trim(line.substr(1)); }
        (void)coissue; /* handled implicitly via write-masks */

        /* opcode = up to first whitespace */
        size_t ws = line.find_first_of(" \t");
        std::string opTok = (ws == std::string::npos) ? line : line.substr(0, ws);
        std::string rest  = (ws == std::string::npos) ? ""   : trim(line.substr(ws));

        /* split opcode into base + result modifiers */
        std::vector<std::string> opParts = split(opTok, '_');
        std::string op = opParts.empty() ? opTok : opParts[0];
        std::vector<std::string> resMods(opParts.begin() + (opParts.empty() ? 0 : 1),
                                         opParts.end());

        std::vector<std::string> operands;
        for (const std::string &o : split(rest, ','))
            if (!trim(o).empty()) operands.push_back(trim(o));

        /* ── tex tN : sample stage N ── */
        if (op == "tex") {
            if (operands.empty()) continue;
            Dest d = parse_dest(operands[0]);
            char n = (d.reg.size() == 2) ? d.reg[1] : '0';
            body << "  " << d.reg << " = tex" << n << ".Sample(samp" << n
                 << ", i.uv" << n << " * texcoord_scale[" << n
                 << "].xy);\n";
            continue;
        }
        if (op == "texcoord" || op == "texkill") {
            /* rare; texcoord = pass-through coord as color. Approximate. */
            if (!operands.empty()) {
                Dest d = parse_dest(operands[0]);
                char n = (d.reg.size() == 2) ? d.reg[1] : '0';
                body << "  " << d.reg << " = float4(i.uv" << n << ", 0, 1);\n";
            }
            continue;
        }

        /* ── xfc/xfd : NV2A final combiner (verified vs d3d8_combiners.c) ──
         * Seven inputs A,B,C,D,E,F,G, NO destination (writes the pixel output r0):
         *   out.rgb = D + A*B + (1-A)*C ;  out.a = G.a
         * EF_PROD = E*F and V1R0_SUM = v1+r0 are computed first and usable as
         * `prod`/`sum` by A-G. Missing trailing operands default to zero. */
        if (op == "xfc" || op == "xfd") {
            auto getop = [&](size_t k) -> Src {
                return k < operands.size() ? parse_src(operands[k]) : parse_src("zero");
            };
            Src A = getop(0), B = getop(1), C = getop(2), D = getop(3),
                E = getop(4), F = getop(5), G = getop(6);
            body << "  prod.rgb = " << emit_src(E, 3) << " * " << emit_src(F, 3) << ";\n";
            body << "  prod.a = "   << emit_src(E, 1) << " * " << emit_src(F, 1) << ";\n";
            body << "  sum = v1 + r0;\n";
            std::string a3 = emit_src(A, 3), b3 = emit_src(B, 3),
                        c3 = emit_src(C, 3), d3 = emit_src(D, 3);
            body << "  r0.rgb = " << d3 << " + " << a3 << " * " << b3
                 << " + (1.0 - " << a3 << ") * " << c3 << ";\n";
            body << "  r0.a = " << emit_src(G, 1) << ";\n";
            continue;
        }

        /* `def cN, ...` handled in the pre-scan above. */
        if (op == "def") continue;

        /* ── Xbox combiner macros (best-effort; the exact NV2A combiner
         * semantics are approximated — good enough to compile and to render the
         * simple 1-2 texture UI shaders correctly; complex world shaders are
         * approximate). Forms:
         *   xmma/xmmc d0, d1, dsum, a, b, c, d   (mul-mul-add / -mux)
         *   xdm       d0, dsum,     a, b, c, d   (component-mul + dot3)
         *   xdd/xbd   d0, d1,       a, b, c, d   (dual dot3)               */
        if (op == "xmma" || op == "xmmc") {
            if (operands.size() >= 7) {
                Dest d0 = parse_dest(operands[0]), d1 = parse_dest(operands[1]),
                     ds = parse_dest(operands[2]);
                Src a = parse_src(operands[3]), b = parse_src(operands[4]),
                    c = parse_src(operands[5]), d = parse_src(operands[6]);
                auto ab = [&](int w){ return "(" + emit_src(a, w) + " * " + emit_src(b, w) + ")"; };
                auto cd = [&](int w){ return "(" + emit_src(c, w) + " * " + emit_src(d, w) + ")"; };
                emit_masked(body, d0, ab(d0.width()), resMods);
                emit_masked(body, d1, cd(d1.width()), resMods);
                std::string sumv = (op == "xmmc")
                    ? "((r0.a > 0.5) ? " + cd(ds.width()) + " : " + ab(ds.width()) + ")"
                    : ab(ds.width()) + " + " + cd(ds.width());
                emit_masked(body, ds, sumv, resMods);
                warn << op << ": Xbox combiner macro (approximate)\n";
            }
            continue;
        }
        if (op == "xdm" || op == "xdd" || op == "xbd") {
            if (operands.size() >= 6) {
                Dest d0 = parse_dest(operands[0]), d1 = parse_dest(operands[1]);
                Src a = parse_src(operands[2]), b = parse_src(operands[3]),
                    c = parse_src(operands[4]), d = parse_src(operands[5]);
                if (op == "xdm") {  /* d0 = a*b (component), dsum = dot3(c,d) */
                    emit_masked(body, d0, emit_src(a, d0.width()) + " * " + emit_src(b, d0.width()), resMods);
                    emit_masked(body, d1, "dot(" + emit_src(c, 3) + ", " + emit_src(d, 3) + ")", resMods);
                } else {            /* dual dot3 */
                    emit_masked(body, d0, "dot(" + emit_src(a, 3) + ", " + emit_src(b, 3) + ")", resMods);
                    emit_masked(body, d1, "dot(" + emit_src(c, 3) + ", " + emit_src(d, 3) + ")", resMods);
                }
                warn << op << ": Xbox combiner macro (approximate)\n";
            }
            continue;
        }
        /* ── arithmetic ops ── */
        if (operands.empty()) continue;
        Dest d = parse_dest(operands[0]);
        int w = d.width();
        std::vector<Src> s;
        for (size_t k = 1; k < operands.size(); k++) s.push_back(parse_src(operands[k]));

        std::string val;
        if (op == "mov" && s.size() >= 1) {
            val = emit_src(s[0], w);
        } else if (op == "add" && s.size() >= 2) {
            val = emit_src(s[0], w) + " + " + emit_src(s[1], w);
        } else if (op == "sub" && s.size() >= 2) {
            val = emit_src(s[0], w) + " - " + emit_src(s[1], w);
        } else if (op == "mul" && s.size() >= 2) {
            val = emit_src(s[0], w) + " * " + emit_src(s[1], w);
        } else if (op == "mad" && s.size() >= 3) {
            val = emit_src(s[0], w) + " * " + emit_src(s[1], w) + " + " + emit_src(s[2], w);
        } else if (op == "lrp" && s.size() >= 3) {
            /* dest = s1*s0 + s2*(1-s0) == lerp(s2, s1, s0) */
            val = "lerp(" + emit_src(s[2], w) + ", " + emit_src(s[1], w) + ", " +
                  emit_src(s[0], w) + ")";
        } else if (op == "cnd" && s.size() >= 3) {
            /* dest = (s0 > 0.5) ? s1 : s2 ; s0 is r0.a in ps.1.1 */
            val = "((" + emit_src(s[0], 1) + " > 0.5) ? (" + emit_src(s[1], w) +
                  ") : (" + emit_src(s[2], w) + "))";
        } else if (op == "dp3" && s.size() >= 2) {
            val = "dot(" + emit_src(s[0], 3) + ", " + emit_src(s[1], 3) + ")";
        } else {
            warn << "unsupported op '" << op << "' (" << line << ")\n";
            res.ok = false;
            continue;
        }

        emit_masked(body, d, val, resMods);
    }

    /* ── assemble full HLSL ── */
    std::ostringstream hlsl;
    hlsl <<
        "Texture2D tex0 : register(t0);\n"
        "Texture2D tex1 : register(t1);\n"
        "Texture2D tex2 : register(t2);\n"
        "Texture2D tex3 : register(t3);\n"
        "SamplerState samp0 : register(s4);\n"
        "SamplerState samp1 : register(s5);\n"
        "SamplerState samp2 : register(s6);\n"
        "SamplerState samp3 : register(s7);\n"
        "cbuffer XPS_Consts : register(b8) {\n"
        "  float4 c[8];\n"
        /* Rows 8..20 pad to the shared texcoord_scale slot, which sits
         * after the combiner CB's fog-parameter row (21 float4s). */
        "  float4 _texcoord_pad[13];\n"
        "  float4 texcoord_scale[4];\n"
        "};\n"
        "struct PSIn {\n"
        "  float4 pos  : SV_Position;\n"
        "  float4 col0 : COLOR0;\n"
        "  float4 col1 : COLOR1;\n"
        "  float2 uv0  : TEXCOORD0;\n"
        "  float2 uv1  : TEXCOORD1;\n"
        "  float2 uv2  : TEXCOORD2;\n"
        "  float2 uv3  : TEXCOORD3;\n"
        "};\n"
        "float4 main(PSIn i) : SV_Target {\n"
        "  float4 r0 = (float4)0, r1 = (float4)0;\n"
        "  float4 t0 = (float4)0, t1 = (float4)0, t2 = (float4)0, t3 = (float4)0;\n"
        "  float4 v0 = i.col0, v1 = i.col1;\n"
        "  float4 prod = (float4)0, sum = (float4)0, discardReg = (float4)0;\n";
    hlsl << body.str();
    hlsl << "  return r0;\n}\n";

    res.hlsl = hlsl.str();
    res.warnings = warn.str();
    return res;
}
