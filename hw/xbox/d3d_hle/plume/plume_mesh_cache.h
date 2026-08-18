#ifndef XGPU_PLUME_MESH_CACHE_H
#define XGPU_PLUME_MESH_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlumeMeshCacheKey {
    uint32_t vb_data_va;
    uint32_t ib_data_va;
    uint32_t index_byte_offset;
    uint32_t index_count;
    uint32_t stride;
    uint32_t base_vertex;
    uint32_t fvf_or_vs;
} PlumeMeshCacheKey;

typedef struct PlumeMeshCacheEntry {
    PlumeMeshCacheKey key;
    uint64_t vb_generation;
    uint64_t ib_generation;
    uint32_t id; /* 0 = vacant */
    uint32_t vertex_count;
    uint32_t index_count;
} PlumeMeshCacheEntry;

void plume_mesh_cache_clear(void);
/* Returns existing id if key matches AND both generations match; 0 otherwise. */
uint32_t plume_mesh_cache_find(const PlumeMeshCacheKey *key,
                               uint64_t vb_generation,
                               uint64_t ib_generation);
/* Inserts or replaces. Returns a non-zero id, or 0 if the table is full. */
uint32_t plume_mesh_cache_store(const PlumeMeshCacheKey *key,
                                uint64_t vb_generation,
                                uint64_t ib_generation,
                                uint32_t vertex_count,
                                uint32_t index_count);
/*
 * Drop every entry whose vb_data_va or ib_data_va equals resource_data_va.
 * If dropped_ids is non-NULL, writes up to *inout_count dropped ids and
 * sets *inout_count to the number written (may be truncated).
 */
void plume_mesh_cache_invalidate_resource(uint32_t resource_data_va,
                                          uint32_t *dropped_ids,
                                          size_t *inout_count);
size_t plume_mesh_cache_count(void);

#ifdef __cplusplus
}
#endif

#endif
