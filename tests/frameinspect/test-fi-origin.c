#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-origin.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

int main(void)
{
    FIOriginNode nodes[3] = {
        {
            .parent = FI_ORIGIN_NODE_INVALID,
        },
        {
            .parent = FI_ORIGIN_NODE_ROOT,
            .call_site = 0x11000,
            .callee = 0x22000,
            .n_argsets = 2,
            .argset_ids = { 0, 1 },
        },
        {
            .parent = 1,
            .call_site = 0x33000,
            .callee = 0x44000,
            .n_argsets = 1,
            .argsets_truncated = 1,
            .argset_ids = { 2 },
        },
    };
    uint32_t argsets[3][6] = {
        { 1, 2, 3, 4, 5, 6 },
        { 7, 8, 9, 10, 11, 12 },
        { 13, 14, 15, 16, 17, 18 },
    };
    uint64_t required = sizeof(nodes) + sizeof(argsets);

    FIOriginSnapshot snapshot;
    CHECK(fi_origin_snapshot_copy(
        &snapshot, nodes, 3, argsets, 3, required,
        FI_ORIGIN_TRUNC_NODES));
    CHECK(snapshot.num_nodes == 3);
    CHECK(snapshot.num_argsets == 3);
    CHECK(snapshot.bytes == required);
    CHECK(snapshot.truncation == FI_ORIGIN_TRUNC_NODES);

    const FIOriginNode *node = fi_origin_snapshot_node(&snapshot, 2);
    CHECK(node != NULL);
    CHECK(node->parent == 1);
    CHECK(node->call_site == 0x33000);
    CHECK(node->callee == 0x44000);
    CHECK(node->argsets_truncated == 1);
    const uint32_t *args = fi_origin_snapshot_argset(&snapshot, node, 0);
    CHECK(args != NULL && args[0] == 13 && args[5] == 18);
    CHECK(fi_origin_snapshot_argset(&snapshot, node, 1) == NULL);
    CHECK(fi_origin_snapshot_node(&snapshot, 3) == NULL);

    /* The finalized snapshot never aliases the live source arrays. */
    nodes[2].callee = 0;
    argsets[2][0] = 0;
    CHECK(node->callee == 0x44000);
    CHECK(args[0] == 13);
    fi_origin_snapshot_free(&snapshot);
    CHECK(snapshot.nodes == NULL && snapshot.argsets == NULL);

    /* A complete snapshot cannot fit: publish no dangling partial graph. */
    CHECK(!fi_origin_snapshot_copy(
        &snapshot, nodes, 3, argsets, 3, required - 1, 0));
    CHECK(snapshot.nodes == NULL && snapshot.argsets == NULL);
    CHECK(snapshot.num_nodes == 0 && snapshot.num_argsets == 0);
    CHECK(snapshot.truncation == FI_ORIGIN_TRUNC_BUDGET);
    fi_origin_snapshot_free(&snapshot);

    CHECK(!fi_origin_snapshot_copy(
        &snapshot, NULL, 1, argsets, 3, required, 0));
    CHECK(snapshot.truncation == FI_ORIGIN_TRUNC_INVALID);
    fi_origin_snapshot_free(&snapshot);

    /* Empty snapshots are valid and need no allocation. */
    CHECK(fi_origin_snapshot_copy(&snapshot, NULL, 0, NULL, 0, 0, 0));
    CHECK(snapshot.bytes == 0 && snapshot.truncation == 0);
    fi_origin_snapshot_free(&snapshot);

    printf("PASS\n");
    return 0;
}
