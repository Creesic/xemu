#include "plume_mesh_cache.h"

#include <string.h>

enum { kPlumeMeshCacheSlots = 1024 };

static PlumeMeshCacheEntry g_slots[kPlumeMeshCacheSlots];
static uint32_t g_next_id = 1;
static size_t g_live = 0;

static uint64_t mesh_cache_hash(const PlumeMeshCacheKey *key)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const uint8_t *bytes = (const uint8_t *)key;
    size_t i;

    for (i = 0; i < sizeof(*key); ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int mesh_cache_key_equal(const PlumeMeshCacheKey *a,
                                const PlumeMeshCacheKey *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static int mesh_cache_probe(const PlumeMeshCacheKey *key, size_t *out_index,
                            int *out_found)
{
    uint64_t hash;
    size_t start;
    size_t i;
    size_t first_empty = (size_t)-1;

    if (!key || !out_index || !out_found)
        return 0;
    hash = mesh_cache_hash(key);
    start = (size_t)(hash % (uint64_t)kPlumeMeshCacheSlots);
    for (i = 0; i < (size_t)kPlumeMeshCacheSlots; ++i) {
        size_t slot = (start + i) % (size_t)kPlumeMeshCacheSlots;
        if (g_slots[slot].id == 0) {
            if (first_empty == (size_t)-1)
                first_empty = slot;
            break;
        }
        if (mesh_cache_key_equal(&g_slots[slot].key, key)) {
            *out_index = slot;
            *out_found = 1;
            return 1;
        }
    }
    if (first_empty == (size_t)-1)
        return 0;
    *out_index = first_empty;
    *out_found = 0;
    return 1;
}

void plume_mesh_cache_clear(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id = 1;
    g_live = 0;
}

uint32_t plume_mesh_cache_find(const PlumeMeshCacheKey *key,
                               uint64_t vb_generation,
                               uint64_t ib_generation)
{
    size_t index = 0;
    int found = 0;

    if (!key || !mesh_cache_probe(key, &index, &found) || !found)
        return 0;
    if (g_slots[index].vb_generation != vb_generation ||
        g_slots[index].ib_generation != ib_generation)
        return 0;
    return g_slots[index].id;
}

uint32_t plume_mesh_cache_store(const PlumeMeshCacheKey *key,
                                uint64_t vb_generation,
                                uint64_t ib_generation,
                                uint32_t vertex_count,
                                uint32_t index_count)
{
    size_t index = 0;
    int found = 0;

    if (!key || !mesh_cache_probe(key, &index, &found))
        return 0;
    if (!found) {
        if (g_next_id == 0)
            g_next_id = 1;
        g_slots[index].id = g_next_id++;
        g_slots[index].key = *key;
        g_live++;
    }
    g_slots[index].vb_generation = vb_generation;
    g_slots[index].ib_generation = ib_generation;
    g_slots[index].vertex_count = vertex_count;
    g_slots[index].index_count = index_count;
    return g_slots[index].id;
}

void plume_mesh_cache_invalidate_resource(uint32_t resource_data_va,
                                          uint32_t *dropped_ids,
                                          size_t *inout_count)
{
    PlumeMeshCacheEntry keep[kPlumeMeshCacheSlots];
    size_t keep_count = 0;
    size_t written = 0;
    size_t cap = inout_count ? *inout_count : 0;
    size_t i;

    if (resource_data_va == 0)
        return;
    for (i = 0; i < (size_t)kPlumeMeshCacheSlots; ++i) {
        if (g_slots[i].id == 0)
            continue;
        if (g_slots[i].key.vb_data_va == resource_data_va ||
            g_slots[i].key.ib_data_va == resource_data_va) {
            if (dropped_ids && written < cap)
                dropped_ids[written] = g_slots[i].id;
            written++;
            continue;
        }
        keep[keep_count++] = g_slots[i];
    }
    memset(g_slots, 0, sizeof(g_slots));
    g_live = 0;
    for (i = 0; i < keep_count; ++i) {
        size_t index = 0;
        int found = 0;
        if (!mesh_cache_probe(&keep[i].key, &index, &found) || found)
            continue;
        g_slots[index] = keep[i];
        g_live++;
    }
    if (inout_count)
        *inout_count = dropped_ids ? (written < cap ? written : cap) : written;
}

size_t plume_mesh_cache_count(void)
{
    return g_live;
}
