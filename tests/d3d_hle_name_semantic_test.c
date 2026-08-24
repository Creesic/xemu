#include "xemu_d3d_hle_name.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_name(const char *raw, const char *expected)
{
    size_t length = xemu_d3d_hle_canonical_name_length(raw);
    assert(length == strlen(expected));
    assert(memcmp(raw, expected, length) == 0);
}

int main(void)
{
    expect_name("D3DDevice_GetBackBuffer2", "D3DDevice_GetBackBuffer2");
    expect_name("CDevice_FreeFrameBuffers_4", "CDevice_FreeFrameBuffers");
    expect_name("D3D_MakeRequestedSpace_8", "D3D_MakeRequestedSpace");
    expect_name("D3DDevice_GetBackBuffer2_0__LTCG_eax1",
                "D3DDevice_GetBackBuffer2");
    expect_name("D3DDevice_SetTextureStageStateNotInline2_0__LTCG_eax1",
                "D3DDevice_SetTextureStageStateNotInline2");
    expect_name("Direct3D_CreateDevice__LTCG_eax4",
                "Direct3D_CreateDevice");
    puts("d3d_hle_name_semantic_test: OK");
    return 0;
}
