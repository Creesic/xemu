/*
 * xemu frame inspector: address -> function-name symbol map
 *
 * Loads a plain-text symbol map (as exported from IDA) and resolves guest
 * addresses to "name (+offset)" for the inspector's Origin / Writers /
 * Methods / Address-Lookup views. Header-only and QEMU-independent so it can
 * be unit-tested standalone.
 *
 * File format, one function per line (# and blank lines ignored):
 *   HEXADDR HEXSIZE NAME
 * The 2-column "HEXADDR NAME" form used by tools/calltrace symbol maps is also
 * accepted (size 0). A middle token is read as SIZE only when it is entirely
 * hexadecimal AND a NAME token follows it, so names that merely start with
 * hex-looking characters are not misparsed.
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_FRAMEINSPECT_SYMBOLS_H
#define XEMU_FRAMEINSPECT_SYMBOLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* For a symbol with no size, cap how far past its start we still attribute it
 * (guards addresses that fall in a gap after the last known function). */
#define FI_SYM_MAX_GAP 0x8000u

typedef struct FISym {
    uint32_t addr;
    uint32_t size;
    char *name;
} FISym;

typedef struct FISymbols {
    FISym *syms;   /* sorted ascending by addr after load */
    uint32_t count, cap;
} FISymbols;

static inline char *fi_sym_dup(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static inline void fi_symbols_free(FISymbols *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        free(s->syms[i].name);
    }
    free(s->syms);
    memset(s, 0, sizeof(*s));
}

static inline int fi_sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const FISym *)a)->addr;
    uint32_t y = ((const FISym *)b)->addr;
    return (x > y) - (x < y);
}

static inline bool fi_sym_is_hex_tok(const char *p, const char *end)
{
    if (end == p) {
        return false;
    }
    for (const char *c = p; c < end; c++) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
              (*c >= 'A' && *c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* Load/replace the symbol table. On open failure returns false and leaves the
 * existing table untouched. On success the previous table is freed. */
static inline bool fi_symbols_load(FISymbols *s, const char *path,
                                   int *out_count)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    FISymbols ns;
    memset(&ns, 0, sizeof(ns));
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }
        char *end;
        unsigned long addr = strtoul(p, &end, 16);
        if (end == p) {
            continue;
        }
        p = end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        /* Optional size token: purely hex, with a name after it. */
        uint32_t size = 0;
        char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t' &&
               *tok_end != '\n' && *tok_end != '\r') {
            tok_end++;
        }
        char *after = tok_end;
        while (*after == ' ' || *after == '\t') {
            after++;
        }
        if (*after && *after != '\n' && *after != '\r' &&
            fi_sym_is_hex_tok(p, tok_end)) {
            size = (uint32_t)strtoul(p, NULL, 16);
            p = after;
        }
        /* Name = rest of line, trimmed. */
        char *nl = p + strlen(p);
        while (nl > p && (nl[-1] == '\n' || nl[-1] == '\r' || nl[-1] == ' ' ||
                          nl[-1] == '\t')) {
            nl--;
        }
        if (nl == p) {
            continue;
        }
        if (ns.count >= ns.cap) {
            uint32_t nc = ns.cap ? ns.cap * 2 : 1024;
            FISym *n = (FISym *)realloc(ns.syms, nc * sizeof(FISym));
            if (!n) {
                fi_symbols_free(&ns);
                fclose(f);
                return false;
            }
            ns.syms = n;
            ns.cap = nc;
        }
        ns.syms[ns.count].addr = (uint32_t)addr;
        ns.syms[ns.count].size = size;
        ns.syms[ns.count].name = fi_sym_dup(p, (size_t)(nl - p));
        if (!ns.syms[ns.count].name) {
            fi_symbols_free(&ns);
            fclose(f);
            return false;
        }
        ns.count++;
    }
    fclose(f);
    qsort(ns.syms, ns.count, sizeof(FISym), fi_sym_cmp);
    fi_symbols_free(s);
    *s = ns;
    if (out_count) {
        *out_count = (int)ns.count;
    }
    return true;
}

/* Return the name of the function containing `addr` (nearest start at or below
 * addr, within its size or FI_SYM_MAX_GAP when size is unknown), or NULL.
 * *out_off, when non-NULL, receives addr - function_start. */
static inline const char *fi_symbols_lookup(const FISymbols *s, uint32_t addr,
                                            uint32_t *out_off)
{
    if (!s->count) {
        return NULL;
    }
    uint32_t lo = 0, hi = s->count; /* first entry with addr > query */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->syms[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return NULL;
    }
    const FISym *sym = &s->syms[lo - 1];
    uint32_t off = addr - sym->addr;
    if (sym->size ? (off >= sym->size) : (off >= FI_SYM_MAX_GAP)) {
        return NULL;
    }
    if (out_off) {
        *out_off = off;
    }
    return sym->name;
}

#endif
