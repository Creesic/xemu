#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-symbols.h"

int main(void)
{
    const char *path = "test-fi-symbols.tmp";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("# a comment\n", f);
    fputs("\n", f);
    fputs("00349DB0 1A0 D3DDevice_DrawVerticesUP\n", f);   /* 3-col: addr size name */
    fputs("001195B6 33 mm3_meme_flush_deferred_draws\n", f);
    fputs("00012000 20 TwoColMissingBelow\n", f);
    fputs("00020000 NoSizeSymbol\n", f);                    /* 2-col: addr name */
    fputs("00030000 40 Name With Spaces\n", f);             /* name may contain spaces */
    fclose(f);

    FISymbols s;
    memset(&s, 0, sizeof(s));
    int n = 0;
    assert(fi_symbols_load(&s, path, &n));
    assert(n == 5 && s.count == 5);

    uint32_t off = 0xdead;

    /* exact function start -> offset 0 */
    const char *nm = fi_symbols_lookup(&s, 0x349DB0, &off);
    assert(nm && strcmp(nm, "D3DDevice_DrawVerticesUP") == 0 && off == 0);

    /* mid-function -> name + offset */
    off = 0xdead;
    nm = fi_symbols_lookup(&s, 0x349DB0 + 0x2a, &off);
    assert(nm && strcmp(nm, "D3DDevice_DrawVerticesUP") == 0 && off == 0x2a);

    /* just past the sized function's end -> no match (gap) */
    nm = fi_symbols_lookup(&s, 0x349DB0 + 0x1a0, NULL);
    assert(nm == NULL);

    /* below the lowest symbol -> no match */
    assert(fi_symbols_lookup(&s, 0x1000, NULL) == NULL);

    /* 2-column (no size) symbol: within the gap guard resolves */
    off = 0xdead;
    nm = fi_symbols_lookup(&s, 0x20000 + 0x100, &off);
    assert(nm && strcmp(nm, "NoSizeSymbol") == 0 && off == 0x100);
    /* ...but beyond FI_SYM_MAX_GAP it does not */
    assert(fi_symbols_lookup(&s, 0x20000 + FI_SYM_MAX_GAP, NULL) == NULL);

    /* name with spaces preserved */
    nm = fi_symbols_lookup(&s, 0x30000, NULL);
    assert(nm && strcmp(nm, "Name With Spaces") == 0);

    /* reload replaces cleanly */
    assert(fi_symbols_load(&s, path, &n) && n == 5);

    fi_symbols_free(&s);
    assert(s.syms == NULL && s.count == 0);
    remove(path);
    printf("PASS\n");
    return 0;
}
