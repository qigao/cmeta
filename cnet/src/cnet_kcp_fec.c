#include "cnet_kcp_fec_internal.h"

#include <gf256.h>
#include "cnet_monocypher_namespace.h"
#include <monocypher.h>

#include <stdlib.h>
#include <string.h>

enum {
  CNET_KCP_FEC_MAX_RECEIVE_GROUPS = 64,
  CNET_KCP_FEC_MAX_TOTAL_SHARDS = 255,
  CNET_KCP_FEC_MAX_STATE_BYTES = 64 * 1024 * 1024,
  CNET_KCP_FEC_VERSION = 1,
  CNET_KCP_FEC_DATA_FRAME = 1,
  CNET_KCP_FEC_PARITY_FRAME = 2
};

static const uint8_t CNET_KCP_FEC_MAGIC[4] = {'T', 'K', 'F', '1'};

typedef struct cnet_kcp_fec_receive_group {
  uint32_t group_id;
  uint16_t received;
  uint8_t **shards;
  uint8_t *present;
} cnet_kcp_fec_receive_group;

struct cnet_kcp_fec_state {
  cnet_kcp_fec_config config;
  miniblas_gf256_rs_t codec;
  cnet_kcp_fec_output_fn output;
  void *output_user;
  uint32_t next_group_id;
  uint16_t next_shard_id;
  size_t shard_size;
  size_t frame_capacity;
  uint8_t *frame;
  uint8_t *encode_data;
  uint8_t *encode_parity;
  uint8_t **encode_shards;
  cnet_kcp_fec_receive_group *receive_groups;
  uint8_t *receive_shard_storage;
  uint8_t **receive_shard_pointers;
  uint8_t *receive_present_storage;
  uint8_t auth_key[CNET_KCP_PSK_BYTES];
  uint64_t session_epoch;
  bool session_ready;
};

static void cnet_kcp_fec_write_u16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)(value >> 8u);
  output[1] = (uint8_t)value;
}

static void cnet_kcp_fec_write_u32(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)(value >> 24u);
  output[1] = (uint8_t)(value >> 16u);
  output[2] = (uint8_t)(value >> 8u);
  output[3] = (uint8_t)value;
}

static void cnet_kcp_fec_write_u64(uint8_t *output, uint64_t value) {
  size_t index;
  for (index = 0u; index < 8u; ++index)
    output[index] = (uint8_t)(value >> (56u - (8u * index)));
}

static uint16_t cnet_kcp_fec_read_u16(const uint8_t *input) {
  return (uint16_t)(((uint16_t)input[0] << 8u) | input[1]);
}

static uint32_t cnet_kcp_fec_read_u32(const uint8_t *input) {
  return ((uint32_t)input[0] << 24u) | ((uint32_t)input[1] << 16u) |
         ((uint32_t)input[2] << 8u) | input[3];
}

static uint64_t cnet_kcp_fec_read_u64(const uint8_t *input) {
  uint64_t value = 0u;
  size_t index;
  for (index = 0u; index < 8u; ++index) value = (value << 8u) | input[index];
  return value;
}

static bool cnet_kcp_fec_mac_equal(const uint8_t *left, const uint8_t *right) {
  uint8_t difference = 0u;
  size_t index;
  for (index = 0u; index < CNET_KCP_FEC_MAC_BYTES; ++index)
    difference |= (uint8_t)(left[index] ^ right[index]);
  return difference == 0u;
}

static int cnet_kcp_fec_map_codec_status(int status) {
  if (status == MINIBLAS_GF256_OK) return SALTS_OK;
  if (status == MINIBLAS_GF256_ENOMEM) return SALTS_ENOMEM;
  return SALTS_EINVAL;
}

int cnet_kcp_fec_config_validate(const cnet_kcp_fec_config *config) {
  size_t total_shards;
  size_t shard_size;
  size_t group_bytes;
  if (config == NULL || config->backend != CNET_KCP_FEC_REED_SOLOMON ||
      config->data_shards == 0u || config->parity_shards == 0u ||
      config->max_payload_bytes == 0u || config->receive_group_count == 0u ||
      config->receive_group_count > CNET_KCP_FEC_MAX_RECEIVE_GROUPS)
    return SALTS_EINVAL;
  total_shards = (size_t)config->data_shards + config->parity_shards;
  if (total_shards > CNET_KCP_FEC_MAX_TOTAL_SHARDS) return SALTS_EINVAL;
  /* Parity covers the stored data length too, and must fit the wire length field. */
  if (config->max_payload_bytes > UINT16_MAX - CNET_KCP_FEC_DATA_LENGTH_BYTES)
    return SALTS_ERANGE;
  shard_size = (size_t)config->max_payload_bytes + CNET_KCP_FEC_DATA_LENGTH_BYTES;
  if (total_shards > SIZE_MAX / shard_size) return SALTS_ERANGE;
  group_bytes = total_shards * shard_size;
  if ((size_t)config->receive_group_count > SIZE_MAX / group_bytes ||
      group_bytes * config->receive_group_count > CNET_KCP_FEC_MAX_STATE_BYTES)
    return SALTS_ERANGE;
  return SALTS_OK;
}

static void cnet_kcp_fec_group_reset(cnet_kcp_fec_state *state,
                                     cnet_kcp_fec_receive_group *group) {
  if (state == NULL || group == NULL || group->shards == NULL || group->present == NULL) return;
  memset(group->present, 0, state->codec.total_shards);
  memset(group->shards[0], 0, (size_t)state->codec.total_shards * state->shard_size);
  group->group_id = 0u;
  group->received = 0u;
}

static cnet_kcp_fec_receive_group *cnet_kcp_fec_group_get(cnet_kcp_fec_state *state,
                                                           uint32_t group_id) {
  cnet_kcp_fec_receive_group *group =
      &state->receive_groups[group_id % state->config.receive_group_count];
  if (group->group_id == group_id) return group;
  cnet_kcp_fec_group_reset(state, group);
  group->group_id = group_id;
  return group;
}

int cnet_kcp_fec_init(const cnet_kcp_fec_config *config, cnet_kcp_fec_output_fn output,
                      void *output_user, cnet_kcp_fec_state **out_state) {
  cnet_kcp_fec_state *state;
  size_t receive_shards;
  size_t index;
  int status;
  if (out_state == NULL) return SALTS_EINVAL;
  *out_state = NULL;
  status = cnet_kcp_fec_config_validate(config);
  if (status != SALTS_OK || output == NULL) return status != SALTS_OK ? status : SALTS_EINVAL;
  state = (cnet_kcp_fec_state *)calloc(1u, sizeof(*state));
  if (state == NULL) return SALTS_ENOMEM;
  state->config = *config;
  state->output = output;
  state->output_user = output_user;
  state->next_group_id = 1u;
  state->shard_size = (size_t)config->max_payload_bytes + CNET_KCP_FEC_DATA_LENGTH_BYTES;
  state->frame_capacity =
      (size_t)config->max_payload_bytes + CNET_KCP_FEC_MAX_WIRE_OVERHEAD_BYTES;
  status = cnet_kcp_fec_map_codec_status(
      miniblas_gf256_rs_init(&state->codec, config->data_shards, config->parity_shards));
  if (status != SALTS_OK) {
    free(state);
    return status;
  }
  receive_shards = (size_t)config->receive_group_count * state->codec.total_shards;
  state->frame = (uint8_t *)malloc(state->frame_capacity);
  state->encode_data = (uint8_t *)calloc(config->data_shards, state->shard_size);
  state->encode_parity = (uint8_t *)calloc(config->parity_shards, state->shard_size);
  state->encode_shards = (uint8_t **)calloc(state->codec.total_shards,
                                             sizeof(*state->encode_shards));
  state->receive_groups = (cnet_kcp_fec_receive_group *)calloc(
      config->receive_group_count, sizeof(*state->receive_groups));
  state->receive_shard_storage = (uint8_t *)calloc(receive_shards, state->shard_size);
  state->receive_shard_pointers =
      (uint8_t **)calloc(receive_shards, sizeof(*state->receive_shard_pointers));
  state->receive_present_storage = (uint8_t *)calloc(receive_shards, sizeof(uint8_t));
  if (state->frame == NULL || state->encode_data == NULL || state->encode_parity == NULL ||
      state->encode_shards == NULL || state->receive_groups == NULL ||
      state->receive_shard_storage == NULL || state->receive_shard_pointers == NULL ||
      state->receive_present_storage == NULL) {
    cnet_kcp_fec_destroy(state);
    return SALTS_ENOMEM;
  }
  for (index = 0u; index < config->data_shards; ++index)
    state->encode_shards[index] = state->encode_data + index * state->shard_size;
  for (index = 0u; index < config->parity_shards; ++index)
    state->encode_shards[config->data_shards + index] =
        state->encode_parity + index * state->shard_size;
  for (index = 0u; index < config->receive_group_count; ++index) {
    cnet_kcp_fec_receive_group *group = &state->receive_groups[index];
    size_t shard;
    group->shards = state->receive_shard_pointers + index * state->codec.total_shards;
    group->present = state->receive_present_storage + index * state->codec.total_shards;
    for (shard = 0u; shard < state->codec.total_shards; ++shard)
      group->shards[shard] =
          state->receive_shard_storage +
          (index * state->codec.total_shards + shard) * state->shard_size;
  }
  *out_state = state;
  return SALTS_OK;
}

int cnet_kcp_fec_set_session(cnet_kcp_fec_state *state, uint64_t session_epoch,
                             const uint8_t key[CNET_KCP_PSK_BYTES]) {
  size_t index;
  if (state == NULL || session_epoch == 0u || key == NULL) return SALTS_EINVAL;
  for (index = 0u; index < state->config.receive_group_count; ++index)
    cnet_kcp_fec_group_reset(state, &state->receive_groups[index]);
  memcpy(state->auth_key, key, sizeof(state->auth_key));
  state->session_epoch = session_epoch;
  state->session_ready = true;
  state->next_group_id = 1u;
  state->next_shard_id = 0u;
  memset(state->encode_data, 0, (size_t)state->config.data_shards * state->shard_size);
  memset(state->encode_parity, 0, (size_t)state->config.parity_shards * state->shard_size);
  return SALTS_OK;
}

static int cnet_kcp_fec_emit(cnet_kcp_fec_state *state, uint8_t type, uint32_t group_id,
                             uint16_t shard_id, const void *data, size_t size) {
  size_t wire_size;
  if (state == NULL || !state->session_ready || data == NULL || size == 0u ||
      size > UINT16_MAX || CNET_KCP_FEC_HEADER_BYTES + size + CNET_KCP_FEC_MAC_BYTES >
                              state->frame_capacity)
    return SALTS_EINVAL;
  wire_size = CNET_KCP_FEC_HEADER_BYTES + size + CNET_KCP_FEC_MAC_BYTES;
  memset(state->frame, 0, CNET_KCP_FEC_HEADER_BYTES);
  memcpy(state->frame, CNET_KCP_FEC_MAGIC, sizeof(CNET_KCP_FEC_MAGIC));
  state->frame[4] = CNET_KCP_FEC_VERSION;
  state->frame[5] = type;
  cnet_kcp_fec_write_u16(state->frame + 6u, CNET_KCP_FEC_HEADER_BYTES);
  cnet_kcp_fec_write_u64(state->frame + 8u, state->session_epoch);
  cnet_kcp_fec_write_u32(state->frame + 16u, group_id);
  cnet_kcp_fec_write_u16(state->frame + 20u, shard_id);
  cnet_kcp_fec_write_u16(state->frame + 22u, state->config.data_shards);
  cnet_kcp_fec_write_u16(state->frame + 24u, state->config.parity_shards);
  cnet_kcp_fec_write_u16(state->frame + 26u, (uint16_t)size);
  cnet_kcp_fec_write_u16(state->frame + 28u, 0u);
  memcpy(state->frame + CNET_KCP_FEC_HEADER_BYTES, data, size);
  crypto_blake2b_keyed(state->frame + CNET_KCP_FEC_HEADER_BYTES + size,
                       CNET_KCP_FEC_MAC_BYTES, state->auth_key, sizeof(state->auth_key),
                       state->frame, CNET_KCP_FEC_HEADER_BYTES + size);
  return state->output(state->output_user, state->frame, wire_size);
}

static int cnet_kcp_fec_send_parity(cnet_kcp_fec_state *state, uint32_t group_id) {
  size_t index;
  int status;
  memset(state->encode_parity, 0, (size_t)state->config.parity_shards * state->shard_size);
  status = cnet_kcp_fec_map_codec_status(
      miniblas_gf256_rs_encode(&state->codec, state->encode_shards, state->shard_size));
  for (index = state->config.data_shards;
       status == SALTS_OK && index < state->codec.total_shards; ++index)
    status = cnet_kcp_fec_emit(state, CNET_KCP_FEC_PARITY_FRAME, group_id, (uint16_t)index,
                               state->encode_shards[index], state->shard_size);
  memset(state->encode_data, 0, (size_t)state->config.data_shards * state->shard_size);
  return status;
}

int cnet_kcp_fec_send(cnet_kcp_fec_state *state, const void *data, size_t size) {
  uint16_t shard_id;
  uint32_t group_id;
  uint8_t *block;
  int status;
  if (state == NULL || !state->session_ready || data == NULL || size == 0u ||
      size > state->config.max_payload_bytes)
    return SALTS_EINVAL;
  group_id = state->next_group_id;
  shard_id = state->next_shard_id;
  block = state->encode_shards[shard_id];
  memset(block, 0, state->shard_size);
  cnet_kcp_fec_write_u16(block, (uint16_t)size);
  memcpy(block + CNET_KCP_FEC_DATA_LENGTH_BYTES, data, size);
  status = cnet_kcp_fec_emit(state, CNET_KCP_FEC_DATA_FRAME, group_id, shard_id, data, size);
  if (status != SALTS_OK) return status;
  ++state->next_shard_id;
  if (state->next_shard_id != state->config.data_shards) return SALTS_OK;
  state->next_shard_id = 0u;
  if (++state->next_group_id == 0u) state->next_group_id = 1u;
  return cnet_kcp_fec_send_parity(state, group_id);
}

static int cnet_kcp_fec_decode(cnet_kcp_fec_state *state, const void *data, size_t size,
                               const uint8_t **out_payload, size_t *out_payload_size,
                               uint8_t *out_type, uint32_t *out_group_id,
                               uint16_t *out_shard_id) {
  const uint8_t *input = (const uint8_t *)data;
  uint8_t expected_mac[CNET_KCP_FEC_MAC_BYTES];
  uint16_t payload_size;
  if (state == NULL || !state->session_ready || input == NULL || out_payload == NULL ||
      out_payload_size == NULL || out_type == NULL || out_group_id == NULL ||
      out_shard_id == NULL || size < CNET_KCP_FEC_HEADER_BYTES + CNET_KCP_FEC_MAC_BYTES ||
      memcmp(input, CNET_KCP_FEC_MAGIC, sizeof(CNET_KCP_FEC_MAGIC)) != 0)
    return SALTS_EPROTO;
  payload_size = cnet_kcp_fec_read_u16(input + 26u);
  if (input[4] != CNET_KCP_FEC_VERSION ||
      (input[5] != CNET_KCP_FEC_DATA_FRAME && input[5] != CNET_KCP_FEC_PARITY_FRAME) ||
      cnet_kcp_fec_read_u16(input + 6u) != CNET_KCP_FEC_HEADER_BYTES ||
      cnet_kcp_fec_read_u64(input + 8u) != state->session_epoch ||
      cnet_kcp_fec_read_u16(input + 22u) != state->config.data_shards ||
      cnet_kcp_fec_read_u16(input + 24u) != state->config.parity_shards ||
      cnet_kcp_fec_read_u16(input + 28u) != 0u ||
      CNET_KCP_FEC_HEADER_BYTES + (size_t)payload_size + CNET_KCP_FEC_MAC_BYTES != size)
    return SALTS_EPROTO;
  crypto_blake2b_keyed(expected_mac, sizeof(expected_mac), state->auth_key,
                       sizeof(state->auth_key), input, CNET_KCP_FEC_HEADER_BYTES + payload_size);
  if (!cnet_kcp_fec_mac_equal(expected_mac,
                              input + CNET_KCP_FEC_HEADER_BYTES + payload_size)) {
    crypto_wipe(expected_mac, sizeof(expected_mac));
    return SALTS_EPERM;
  }
  crypto_wipe(expected_mac, sizeof(expected_mac));
  *out_type = input[5];
  *out_group_id = cnet_kcp_fec_read_u32(input + 16u);
  *out_shard_id = cnet_kcp_fec_read_u16(input + 20u);
  *out_payload = input + CNET_KCP_FEC_HEADER_BYTES;
  *out_payload_size = payload_size;
  return SALTS_OK;
}

static int cnet_kcp_fec_store(cnet_kcp_fec_state *state,
                              cnet_kcp_fec_receive_group *group, uint16_t shard_id,
                              const uint8_t *payload, size_t payload_size, bool data_frame) {
  uint8_t *block;
  if (shard_id >= state->codec.total_shards || group->present[shard_id]) return SALTS_OK;
  if ((data_frame && payload_size > state->config.max_payload_bytes) ||
      (!data_frame && payload_size != state->shard_size))
    return SALTS_EPROTO;
  block = group->shards[shard_id];
  memset(block, 0, state->shard_size);
  if (data_frame) {
    cnet_kcp_fec_write_u16(block, (uint16_t)payload_size);
    memcpy(block + CNET_KCP_FEC_DATA_LENGTH_BYTES, payload, payload_size);
  } else {
    memcpy(block, payload, payload_size);
  }
  group->present[shard_id] = 1u;
  ++group->received;
  return SALTS_OK;
}

int cnet_kcp_fec_input(cnet_kcp_fec_state *state, const void *data, size_t size,
                       cnet_kcp_fec_deliver_fn deliver, void *deliver_user) {
  cnet_kcp_fec_receive_group *group;
  const uint8_t *payload;
  size_t payload_size;
  uint32_t group_id;
  uint16_t shard_id;
  uint8_t type;
  uint8_t original_present[CNET_KCP_FEC_MAX_TOTAL_SHARDS];
  size_t index;
  int status;
  if (state == NULL || deliver == NULL) return SALTS_EINVAL;
  status = cnet_kcp_fec_decode(state, data, size, &payload, &payload_size, &type, &group_id,
                               &shard_id);
  if (status != SALTS_OK) return status;
  if ((type == CNET_KCP_FEC_DATA_FRAME && shard_id >= state->config.data_shards) ||
      (type == CNET_KCP_FEC_PARITY_FRAME &&
       (shard_id < state->config.data_shards || shard_id >= state->codec.total_shards)))
    return SALTS_EPROTO;
  group = cnet_kcp_fec_group_get(state, group_id);
  if (type == CNET_KCP_FEC_DATA_FRAME) {
    status = deliver(deliver_user, payload, payload_size);
    if (status != SALTS_OK) return status;
    status = cnet_kcp_fec_store(state, group, shard_id, payload, payload_size, true);
  } else {
    status = cnet_kcp_fec_store(state, group, shard_id, payload, payload_size, false);
  }
  if (status != SALTS_OK || group->received < state->config.data_shards) return status;
  memcpy(original_present, group->present, state->codec.total_shards);
  status = cnet_kcp_fec_map_codec_status(miniblas_gf256_rs_reconstruct_data(
      &state->codec, group->shards, group->present, state->shard_size));
  if (status == SALTS_OK) {
    for (index = 0u; index < state->config.data_shards; ++index) {
      if (original_present[index] == 0u) {
        const uint16_t recovered_size = cnet_kcp_fec_read_u16(group->shards[index]);
        if (recovered_size == 0u || recovered_size > state->config.max_payload_bytes) {
          status = SALTS_EPROTO;
          break;
        }
        status = deliver(deliver_user, group->shards[index] + CNET_KCP_FEC_DATA_LENGTH_BYTES,
                         recovered_size);
        if (status != SALTS_OK) break;
      }
    }
  }
  cnet_kcp_fec_group_reset(state, group);
  return status;
}

void cnet_kcp_fec_destroy(cnet_kcp_fec_state *state) {
  size_t index;
  if (state == NULL) return;
  if (state->receive_groups != NULL) {
    for (index = 0u; index < state->config.receive_group_count; ++index)
      cnet_kcp_fec_group_reset(state, &state->receive_groups[index]);
  }
  free(state->frame);
  free(state->encode_shards);
  free(state->encode_parity);
  free(state->encode_data);
  free(state->receive_present_storage);
  free(state->receive_shard_pointers);
  free(state->receive_shard_storage);
  free(state->receive_groups);
  crypto_wipe(state->auth_key, sizeof(state->auth_key));
  miniblas_gf256_rs_destroy(&state->codec);
  free(state);
}
