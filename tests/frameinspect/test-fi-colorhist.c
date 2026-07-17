#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../xemu-frameinspect-colorhist.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* 4x4 image, keyframe every 8 events */
    FIColorHist ch;
    CHECK(fi_colorhist_init(&ch, 4, 4, 8));

    uint32_t base[16];
    for (int i = 0; i < 16; i++) base[i] = 0x00000000u;
    CHECK(fi_colorhist_set_baseline(&ch, base));
    CHECK(!fi_colorhist_set_baseline(&ch, base));

    /* event A (id 10): set pixels 5,6,7 to 0xAA */
    uint32_t imgA[16];
    memcpy(imgA, base, sizeof(base));
    imgA[5] = imgA[6] = imgA[7] = 0xAAAAAAAAu;
    CHECK(fi_colorhist_add_event(&ch, 10, imgA));

    /* event B (id 11): change pixel 6 to 0xBB */
    uint32_t imgB[16];
    memcpy(imgB, imgA, sizeof(imgA));
    imgB[6] = 0xBBBBBBBBu;
    CHECK(fi_colorhist_add_event(&ch, 11, imgB));

    CHECK(fi_colorhist_num_events(&ch) == 2);

    /* reconstruct after event 0 (A) equals imgA exactly */
    uint32_t out[16];
    CHECK(fi_colorhist_reconstruct(&ch, 0, out));
    CHECK(memcmp(out, imgA, sizeof(out)) == 0);

    /* reconstruct after event 1 (B) equals imgB exactly */
    CHECK(fi_colorhist_reconstruct(&ch, 1, out));
    CHECK(memcmp(out, imgB, sizeof(out)) == 0);
    CHECK(!fi_colorhist_reconstruct(&ch, 2, out));

    /* pixel 6 history: (A: 0x00->0xAA), (B: 0xAA->0xBB) */
    FIColorTouch t[8];
    int n = fi_colorhist_pixel_history(&ch, 6, t, 8);
    CHECK(n == 2);
    CHECK(t[0].event_id == 10 && t[0].before == 0 &&
          t[0].after == 0xAAAAAAAAu);
    CHECK(t[1].event_id == 11 && t[1].before == 0xAAAAAAAAu &&
          t[1].after == 0xBBBBBBBBu);

    /* pixel 5 changed only in A */
    n = fi_colorhist_pixel_history(&ch, 5, t, 8);
    CHECK(n == 1 && t[0].event_id == 10);

    /* pixel 0 never changed */
    n = fi_colorhist_pixel_history(&ch, 0, t, 8);
    CHECK(n == 0);
    CHECK(fi_colorhist_pixel_history(&ch, 16, t, 8) == 0);

    /* keyframe reconstruction: push >8 events so a keyframe is taken,
     * then reconstruct an event after it and verify exactness */
    uint32_t img[16];
    memcpy(img, imgB, sizeof(imgB));
    for (int e = 0; e < 12; e++) {
        img[e % 16] = 0xC0000000u + (uint32_t)e;
        CHECK(fi_colorhist_add_event(&ch, 100u + (uint32_t)e, img));
    }
    uint32_t ref[16];
    memcpy(ref, img, sizeof(img)); /* img now equals state after last event */
    CHECK(fi_colorhist_reconstruct(&ch, fi_colorhist_num_events(&ch) - 1,
                                   out));
    CHECK(memcmp(out, ref, sizeof(out)) == 0);

    fi_colorhist_free(&ch);
    CHECK(ch.events == NULL);

    /* Invalid dimensions and missing baselines fail closed. */
    FIColorHist invalid;
    CHECK(!fi_colorhist_init(&invalid, 0, 1, 8));
    CHECK(!fi_colorhist_init(&invalid, UINT32_MAX, 2, 8));
    CHECK(fi_colorhist_init(&invalid, 2, 2, 8));
    uint32_t small[4] = {1, 2, 3, 4};
    CHECK(!fi_colorhist_add_event(&invalid, 1, small));
    invalid.byte_budget = invalid.bytes_used;
    CHECK(!fi_colorhist_set_baseline(&invalid, small));
    CHECK(!invalid.has_baseline);
    fi_colorhist_free(&invalid);

    /* A failure in a later run rolls back every run in that event. */
    FIColorHist rollback;
    CHECK(fi_colorhist_init(&rollback, 4098, 1, 8));
    uint32_t *zero = (uint32_t *)calloc(rollback.npix, sizeof(uint32_t));
    uint32_t *changed = (uint32_t *)calloc(rollback.npix, sizeof(uint32_t));
    CHECK(zero && changed);
    CHECK(fi_colorhist_set_baseline(&rollback, zero));
    for (uint32_t i = 0; i < 2048; i++) {
        changed[i] = 1;
    }
    changed[2049] = 2;
    rollback.byte_budget = rollback.bytes_used;
    CHECK(!fi_colorhist_add_event(&rollback, 2, changed));
    CHECK(rollback.truncated);
    CHECK(rollback.num_events == 0);
    CHECK(rollback.num_runs == 0);
    CHECK(rollback.num_colors == 0);
    CHECK(memcmp(rollback.current, zero,
                 rollback.npix * sizeof(uint32_t)) == 0);
    free(zero);
    free(changed);
    fi_colorhist_free(&rollback);

    printf("PASS\n");
    return 0;
}
