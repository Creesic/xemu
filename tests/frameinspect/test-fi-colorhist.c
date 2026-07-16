#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-colorhist.h"

int main(void)
{
    /* 4x4 image, keyframe every 8 events */
    FIColorHist ch;
    assert(fi_colorhist_init(&ch, 4, 4, 8));

    uint32_t base[16];
    for (int i = 0; i < 16; i++) base[i] = 0x00000000u;
    fi_colorhist_set_baseline(&ch, base);

    /* event A (id 10): set pixels 5,6,7 to 0xAA */
    uint32_t imgA[16];
    memcpy(imgA, base, sizeof(base));
    imgA[5] = imgA[6] = imgA[7] = 0xAAAAAAAAu;
    assert(fi_colorhist_add_event(&ch, 10, imgA));

    /* event B (id 11): change pixel 6 to 0xBB */
    uint32_t imgB[16];
    memcpy(imgB, imgA, sizeof(imgA));
    imgB[6] = 0xBBBBBBBBu;
    assert(fi_colorhist_add_event(&ch, 11, imgB));

    assert(fi_colorhist_num_events(&ch) == 2);

    /* reconstruct after event 0 (A) equals imgA exactly */
    uint32_t out[16];
    fi_colorhist_reconstruct(&ch, 0, out);
    assert(memcmp(out, imgA, sizeof(out)) == 0);

    /* reconstruct after event 1 (B) equals imgB exactly */
    fi_colorhist_reconstruct(&ch, 1, out);
    assert(memcmp(out, imgB, sizeof(out)) == 0);

    /* pixel 6 history: (A: 0x00->0xAA), (B: 0xAA->0xBB) */
    FIColorTouch t[8];
    int n = fi_colorhist_pixel_history(&ch, 6, t, 8);
    assert(n == 2);
    assert(t[0].event_id == 10 && t[0].before == 0 && t[0].after == 0xAAAAAAAAu);
    assert(t[1].event_id == 11 && t[1].before == 0xAAAAAAAAu &&
           t[1].after == 0xBBBBBBBBu);

    /* pixel 5 changed only in A */
    n = fi_colorhist_pixel_history(&ch, 5, t, 8);
    assert(n == 1 && t[0].event_id == 10);

    /* pixel 0 never changed */
    n = fi_colorhist_pixel_history(&ch, 0, t, 8);
    assert(n == 0);

    /* keyframe reconstruction: push >8 events so a keyframe is taken,
     * then reconstruct an event after it and verify exactness */
    uint32_t img[16];
    memcpy(img, imgB, sizeof(imgB));
    for (int e = 0; e < 12; e++) {
        img[e % 16] = 0xC0000000u + (uint32_t)e;
        assert(fi_colorhist_add_event(&ch, 100u + (uint32_t)e, img));
    }
    uint32_t ref[16];
    memcpy(ref, img, sizeof(img)); /* img now equals state after last event */
    fi_colorhist_reconstruct(&ch, fi_colorhist_num_events(&ch) - 1, out);
    assert(memcmp(out, ref, sizeof(out)) == 0);

    fi_colorhist_free(&ch);
    assert(ch.events == NULL);
    printf("PASS\n");
    return 0;
}
