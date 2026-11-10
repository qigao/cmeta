#ifndef TINYMOCK_REFLECT_H
#define TINYMOCK_REFLECT_H

#include <cmeta/data.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TINYMOCK_REFLECT_MAX_DEPTH = 32,
  TINYMOCK_REFLECT_PATH_BYTES = 256,
  TINYMOCK_REFLECT_DEFAULT_NODES = 1024,
  TINYMOCK_REFLECT_DEFAULT_BYTES = 64 * 1024
};

typedef struct tinymock_reflect_limits {
  size_t max_depth;
  size_t max_nodes;
  size_t max_bytes;
} tinymock_reflect_limits;

typedef struct tinymock_reflect_result {
  bool equal;
  char path[TINYMOCK_REFLECT_PATH_BYTES];
} tinymock_reflect_result;

struct tinymock_cmeta_history;

/* Compare published fields, never padding or unreflected native state.
 * Supports fixed-layout STRUCT, BOOL/SINT/UINT/FLOAT, provider-backed ENUM,
 * STRING/BYTES, and CUSTOM with an explicit storage equality trait. Scalars
 * use canonical equality (including CMeta NaN/signed-zero semantics), without
 * a tolerance. Containers/variants and pointer identity are separate contracts.
 *
 * CMETA_OK means the comparison succeeded; inspect result->equal. On mismatch,
 * path is the first field in declaration order, rooted at "$" or the history
 * parameter name. On error, equal is false and path empty. Missing comparison
 * authority returns TRAIT_MISSING, incompatible history storage TYPE_MISMATCH,
 * and depth/node/byte/path exhaustion CAPACITY_EXCEEDED; provider errors propagate.
 * NULL limits selects defaults; max_depth must be 1..MAX_DEPTH, other limits
 * nonzero. Bytes count both leaf inputs; callback-internal work is provider-owned.
 *
 * Buffer providers must keep both read views valid through this comparison.
 * All inputs are immutable borrows for this call. No allocation, snapshot,
 * retain or cleanup is performed. Keep history/objects/providers and outer
 * Plugin leases alive; serialize against mutation/reset/destroy. O(visited
 * fields + compared bytes), plus canonical descriptor validation/provider cost;
 * stack is bounded by MAX_DEPTH and path storage by PATH_BYTES.
 */
cmeta_status tinymock_cmeta_data_match(const cmeta_data_desc *data,
    const void *actual, const void *expected, const tinymock_reflect_limits *limits,
    tinymock_reflect_result *result);

/* Match an existing VALUE snapshot. Pointer snapshots are rejected, never
 * dereferenced or promoted into snapshots of their pointees. Snapshot copy and
 * destruction still require the history's existing TypeDesc traits contract. */
cmeta_status tinymock_cmeta_history_arg_match_data(
    const struct tinymock_cmeta_history *history, size_t call_index, size_t param_index,
    const cmeta_data_desc *data, const void *expected,
    const tinymock_reflect_limits *limits, tinymock_reflect_result *result);
cmeta_status tinymock_cmeta_history_arg_match_data_name(
    const struct tinymock_cmeta_history *history, size_t call_index, const char *param_name,
    const cmeta_data_desc *data, const void *expected,
    const tinymock_reflect_limits *limits, tinymock_reflect_result *result);

#ifdef __cplusplus
}
#endif

#endif
