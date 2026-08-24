#ifndef HW_XBOX_D3D_HLE_XEMU_D3D_HLE_NAME_H
#define HW_XBOX_D3D_HLE_XEMU_D3D_HLE_NAME_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* XbSymbolDatabase appends either a stack-byte suffix (`_4`) or an LTCG ABI
 * suffix (`_0__LTCG_eax1`). Strip only an underscore-delimited numeric suffix;
 * API version digits such as GetBackBuffer2 remain part of the canonical name. */
static inline size_t xemu_d3d_hle_canonical_name_length(const char *name)
{
    const char *marker;
    const char *end;
    const char *digits;

    if (!name)
        return 0;
    marker = strstr(name, "__LTCG");
    end = marker ? marker : name + strlen(name);
    digits = end;
    while (digits > name && isdigit((unsigned char)digits[-1]))
        --digits;
    if (digits < end && digits > name && digits[-1] == '_')
        return (size_t)(digits - 1 - name);
    return (size_t)(end - name);
}

#endif
