#include "reliable.h"

#include <limits.h>
#include <string.h>

enum { RL_DATA = 1, RL_ACK = 2 };
static const uint8_t wire_magic[4] = { 'S', 'R', 'E', '2' };

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static uint32_t crc_step(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
  }
  return crc;
}
static uint32_t checksum(const uint8_t *p, size_t n) {
  uint32_t crc = crc_step(UINT32_MAX, p, 32u);
  if (n > RL_WIRE_HEADER_BYTES)
    crc = crc_step(crc, p + RL_WIRE_HEADER_BYTES, n - RL_WIRE_HEADER_BYTES);
  return ~crc;
}
static bool key_present(const uint8_t key[RL_KEY_BYTES]) {
  uint8_t nz = 0u;
  for (size_t i = 0; i < RL_KEY_BYTES; ++i) nz |= key[i];
  return nz != 0u;
}
static void secure_wipe(uint8_t *p, size_t count) {
  volatile uint8_t *dest = (volatile uint8_t *)p;
  while (count--) *dest++ = 0u;
}
static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t size) {
  uint8_t difference = 0u;
  for (size_t i = 0; i < size; ++i) difference |= (uint8_t)(a[i] ^ b[i]);
  return difference == 0u;
}
static uint64_t plus_ms(uint64_t t, uint32_t delay) {
  return UINT64_MAX - t < delay ? UINT64_MAX : t + delay;
}
static uint8_t mask_for(uint8_t count) {
  return (uint8_t)((1u << count) - 1u);
}
static uint8_t population(uint8_t mask) {
  uint8_t n = 0;
  while (mask) { n += (uint8_t)(mask & 1u); mask >>= 1; }
  return n;
}
static rl_result validate(rl_session *s, rl_ticket t) {
  if (!s || s->state == RL_EMPTY) return RL_INVALID;
  if (!pthread_equal(s->owner, pthread_self())) return RL_WRONG_OWNER;
  if (s->generation != t.generation || s->state == RL_STOPPED) return RL_STALE;
  return s->in_call ? RL_BUSY : RL_OK;
}
static rl_result clock_advance(rl_session *s, uint64_t now_ms) {
  if (now_ms < s->now_ms) return RL_INVALID;
  s->now_ms = now_ms;
  return RL_OK;
}
static void terminal(rl_session *s, rl_terminal result) {
  uint64_t tag = s->tx.tag;
  memset(&s->tx, 0, sizeof(s->tx));
  if (result == RL_TERMINAL_ACKED) ++s->stats.terminal_acked;
  else ++s->stats.terminal_failed;
  /* Clear active state before the borrowed callback; reentrancy is rejected. */
  s->callbacks.on_terminal(s->callbacks.user, tag, result);
}
static size_t serialize(const rl_session *s, uint8_t *packet, unsigned type,
                        uint32_t message, uint8_t index, uint8_t count,
                        uint16_t total_size, uint8_t mask,
                        const uint8_t *payload, uint16_t payload_size) {
  size_t packet_size = RL_WIRE_HEADER_BYTES + payload_size;
  memset(packet, 0, RL_WIRE_HEADER_BYTES);
  memcpy(packet, wire_magic, 4);
  packet[4] = 2u;
  packet[5] = (uint8_t)type;
  packet[6] = index;
  packet[7] = count;
  put32(packet + 8, s->config.local_id);
  put32(packet + 12, s->config.peer_id);
  put32(packet + 16, s->config.epoch);
  put32(packet + 20, message);
  put16(packet + 24, total_size);
  put16(packet + 26, payload_size);
  put32(packet + 28, mask);
  if (payload_size) memcpy(packet + RL_WIRE_HEADER_BYTES, payload, payload_size);
  put32(packet + 32, checksum(packet, packet_size));
  if (!s->callbacks.mac(s->callbacks.mac_user, s->config.tx_auth_key,
                        packet, packet_size, packet + packet_size)) return 0;
  return packet_size + RL_MAC_BYTES;
}

rl_result rl_start(rl_session *s, const rl_config *config, rl_callbacks callbacks,
                   uint64_t generation, rl_ticket *out_ticket) {
  if (!s || !config || !out_ticket || !callbacks.emit || !callbacks.on_deliver ||
      !callbacks.on_terminal || !callbacks.mac || !callbacks.mac_user || !generation ||
      !config->local_id || !config->peer_id || config->local_id == config->peer_id ||
      !config->epoch || !config->first_message_id ||
      !config->window || config->window > RL_MAX_FRAGMENTS ||
      !config->max_retries || config->max_retries > 16u ||
      !config->rto_ms || config->deadline_ms < config->rto_ms ||
      config->ack_delay_ms >= config->deadline_ms ||
      !key_present(config->tx_auth_key) ||
      !key_present(config->rx_auth_key) ||
      constant_time_equal(config->tx_auth_key, config->rx_auth_key, RL_KEY_BYTES))
    return RL_INVALID;
  if (s->state != RL_EMPTY && s->state != RL_STOPPED) return RL_BUSY;
  if (generation <= s->generation ||
      (s->state == RL_STOPPED && s->config.epoch == config->epoch))
    return RL_STALE;
  memset(s, 0, sizeof(*s));
  s->config = *config;
  s->callbacks = callbacks;
  s->generation = generation;
  s->owner = pthread_self();
  s->tx_next_id = config->first_message_id;
  s->rx_next_id = config->first_message_id;
  s->cwnd = config->window;
  s->state = RL_OPEN;
  out_ticket->generation = generation;
  return RL_OK;
}

rl_result rl_send(rl_session *s, rl_ticket t, const void *data, size_t size,
                  uint64_t tag, uint64_t now_ms) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if (s->state != RL_OPEN) return RL_SEALED;
  if (!data || !size || size > RL_MAX_MESSAGE_BYTES) return RL_INVALID;
  if (s->tx.active) return RL_FULL;
  if (!s->tx_next_id) return RL_EXHAUSTED; /* rollover requires a new epoch */
  if ((st = clock_advance(s, now_ms)) != RL_OK) return st;
  memmove(s->tx.bytes, data, size);
  s->tx.active = true;
  s->tx.size = (uint16_t)size;
  s->tx.tag = tag;
  s->tx.message_id = s->tx_next_id++;
  s->tx.count = (uint8_t)((size + RL_FRAGMENT_BYTES - 1u) / RL_FRAGMENT_BYTES);
  s->tx.admitted_at = now_ms;
  return RL_OK;
}

static void schedule_ack(rl_session *s, uint32_t message, uint16_t size,
                         uint8_t count, uint8_t mask, uint64_t now_ms) {
  bool same = s->ack.pending && s->ack.message_id == message &&
              s->ack.mask == mask;
  s->ack.message_id = message;
  s->ack.size = size;
  s->ack.count = count;
  s->ack.mask = mask;
  s->ack.pending = true;
  if (!same) s->ack.due_at = plus_ms(now_ms, s->config.ack_delay_ms);
}

rl_result rl_input(rl_session *s, rl_ticket t, const void *wire, size_t size,
                   uint64_t now_ms) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if ((st = clock_advance(s, now_ms)) != RL_OK) return st;
  if (!wire || size < RL_WIRE_HEADER_BYTES + RL_MAC_BYTES || size > RL_MAX_WIRE_BYTES) {
    ++s->stats.rx_invalid;
    return RL_PROTOCOL;
  }
  const uint8_t *p = (const uint8_t *)wire;
  uint8_t type = p[5], index = p[6], count = p[7];
  uint32_t msg = get32(p + 20), mask = get32(p + 28);
  uint16_t total = get16(p + 24), payload = get16(p + 26);
  if (memcmp(p, wire_magic, 4) || p[4] != 2u ||
      (type != RL_DATA && type != RL_ACK) ||
      !count || count > RL_MAX_FRAGMENTS || !msg ||
      !total || total > RL_MAX_MESSAGE_BYTES ||
      (uint8_t)((total + RL_FRAGMENT_BYTES - 1u) / RL_FRAGMENT_BYTES) != count ||
      get32(p + 8) != s->config.peer_id ||
      get32(p + 12) != s->config.local_id ||
      get32(p + 16) != s->config.epoch ||
      size != (size_t)RL_WIRE_HEADER_BYTES + payload + RL_MAC_BYTES ||
      checksum(p, size - RL_MAC_BYTES) != get32(p + 32)) {
    ++s->stats.rx_invalid;
    return RL_PROTOCOL;
  }
  uint8_t computed[RL_MAC_BYTES] = {0};
  /* The external authenticator is a synchronous borrowed callback: fence
   * nested session operations just as we do for emit and terminal hooks. */
  s->in_call = true;
  bool verified = s->callbacks.mac(s->callbacks.mac_user, s->config.rx_auth_key,
                                   p, size - RL_MAC_BYTES, computed);
  s->in_call = false;
  bool matches = constant_time_equal(computed, p + size - RL_MAC_BYTES,
                                     RL_MAC_BYTES);
  secure_wipe(computed, sizeof(computed));
  if (!verified) {
    ++s->stats.rx_auth_failed;
    return RL_CRYPTO;
  }
  if (!matches) {
    ++s->stats.rx_auth_failed;
    return RL_AUTH_FAILED;
  }
  if (type == RL_ACK) {
    if (index || payload || !mask || (mask & ~(uint32_t)mask_for(count))) {
      ++s->stats.rx_invalid;
      return RL_PROTOCOL;
    }
    if (!s->tx.active || msg != s->tx.message_id ||
        total != s->tx.size || count != s->tx.count ||
        (mask & ~(uint32_t)s->tx.sent_mask)) return RL_OK;
    uint8_t newly = (uint8_t)mask & (uint8_t)~s->tx.acked_mask;
    s->tx.acked_mask |= (uint8_t)mask;
    if (newly && s->cwnd < s->config.window) ++s->cwnd;
    if (s->tx.acked_mask == mask_for(s->tx.count)) {
      s->in_call = true;
      terminal(s, RL_TERMINAL_ACKED);
      s->in_call = false;
    }
    return RL_OK;
  }
  if (s->state != RL_OPEN) return RL_SEALED;
  size_t offset = (size_t)index * RL_FRAGMENT_BYTES;
  size_t expected = (size_t)total - ((size_t)count - 1u) * RL_FRAGMENT_BYTES;
  if (index >= count || mask || payload == 0 ||
      payload != (index == count - 1u ? expected : RL_FRAGMENT_BYTES)) {
    ++s->stats.rx_invalid;
    return RL_PROTOCOL;
  }
  if (msg == s->completed_ack.message_id && s->completed_ack.count) {
    if (total != s->completed_ack.size || count != s->completed_ack.count) {
      ++s->stats.rx_invalid;
      return RL_PROTOCOL;
    }
    ++s->stats.rx_duplicate;
    schedule_ack(s, msg, s->completed_ack.size, s->completed_ack.count,
                 s->completed_ack.mask, now_ms);
    return RL_OK;
  }
  if (msg != s->rx_next_id) return RL_OK; /* bounded one-message RX window */
  if (!s->rx.mask) { s->rx.size = total; s->rx.count = count; }
  if (s->rx.size != total || s->rx.count != count) {
    ++s->stats.rx_invalid;
    return RL_PROTOCOL;
  }
  uint8_t bit = (uint8_t)(1u << index);
  if (s->rx.mask & bit) ++s->stats.rx_duplicate;
  else {
    memcpy(s->rx.bytes + offset, p + RL_WIRE_HEADER_BYTES, payload);
    s->rx.mask |= bit;
  }
  schedule_ack(s, msg, total, count, s->rx.mask, now_ms);
  if (s->rx.mask == mask_for(s->rx.count)) {
    /* Complete-receive identity is recorded before callback, guarding
     * duplicate delivery. Payload is a borrowed view for callback only. */
    s->completed_ack = s->ack;
    s->rx_next_id++;
    s->in_call = true;
    s->callbacks.on_deliver(s->callbacks.user, msg, s->rx.bytes, s->rx.size);
    s->in_call = false;
    s->rx.mask = 0;
    s->rx.size = 0;
    s->rx.count = 0;
  }
  return RL_OK;
}

rl_result rl_step(rl_session *s, rl_ticket t, uint64_t now_ms,
                  size_t budget, size_t *out_emitted) {
  if (!out_emitted) return RL_INVALID;
  *out_emitted = 0;
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if ((st = clock_advance(s, now_ms)) != RL_OK) return st;
  s->in_call = true;
  if (s->tx.active && now_ms >= plus_ms(s->tx.admitted_at, s->config.deadline_ms)) {
    terminal(s, RL_TERMINAL_TIMEOUT);
    goto done;
  }
  if (!budget) goto done;
  if (s->ack.pending && now_ms >= s->ack.due_at) {
    uint8_t packet[RL_MAX_WIRE_BYTES];
    size_t len = serialize(s, packet, RL_ACK, s->ack.message_id, 0u, s->ack.count,
                           s->ack.size, s->ack.mask, NULL, 0u);
    if (!len) { ++s->stats.tx_crypto_failed; st = RL_CRYPTO; goto done; }
    int accepted = s->callbacks.emit(s->callbacks.user, packet, len);
    if (accepted != 0 && accepted != 1) { st = RL_EMIT_FAILED; goto done; }
    if (!accepted) goto done;
    s->ack.pending = false;
    ++s->stats.tx_acks;
    ++*out_emitted;
  }
  if (!s->tx.active) goto done;
  for (unsigned i = 0; i < s->tx.count && *out_emitted < budget; ++i) {
    uint8_t bit = (uint8_t)(1u << i);
    if (s->tx.acked_mask & bit) continue;
    bool retransmit = (s->tx.sent_mask & bit) != 0;
    if (retransmit && now_ms < plus_ms(s->tx.last_sent[i], s->config.rto_ms)) continue;
    if (retransmit && s->tx.tries[i] >= (unsigned)s->config.max_retries + 1u) {
      terminal(s, RL_TERMINAL_TIMEOUT);
      goto done;
    }
    if (!retransmit && population((uint8_t)(s->tx.sent_mask &
                                 (uint8_t)~s->tx.acked_mask)) >= s->cwnd) continue;
    uint8_t packet[RL_MAX_WIRE_BYTES];
    size_t offset = (size_t)i * RL_FRAGMENT_BYTES;
    uint16_t len = (uint16_t)(s->tx.size - offset);
    if (len > RL_FRAGMENT_BYTES) len = RL_FRAGMENT_BYTES;
    size_t packet_len = serialize(s, packet, RL_DATA, s->tx.message_id,
                                  (uint8_t)i, s->tx.count, s->tx.size, 0,
                                  s->tx.bytes + offset, len);
    if (!packet_len) {
      ++s->stats.tx_crypto_failed;
      terminal(s, RL_TERMINAL_EMIT_FAILED);
      st = RL_CRYPTO;
      goto done;
    }
    int accepted = s->callbacks.emit(s->callbacks.user, packet, packet_len);
    if (accepted != 0 && accepted != 1) {
      terminal(s, RL_TERMINAL_EMIT_FAILED);
      st = RL_EMIT_FAILED;
      goto done;
    }
    if (!accepted) break; /* link backpressure transfers no ownership */
    if (retransmit) {
      ++s->stats.tx_retransmitted;
      ++s->stats.congestion_reduced;
      s->cwnd = s->cwnd > 1u ? (uint8_t)(s->cwnd / 2u) : 1u;
    }
    ++s->stats.tx_data;
    s->tx.tries[i]++;
    s->tx.last_sent[i] = now_ms;
    s->tx.sent_mask |= bit;
    ++*out_emitted;
  }
done:
  s->in_call = false;
  return st;
}

rl_result rl_cancel(rl_session *s, rl_ticket t) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if (!s->tx.active) return RL_OK;
  s->in_call = true;
  terminal(s, RL_TERMINAL_CANCELLED);
  s->in_call = false;
  return RL_OK;
}
rl_result rl_seal(rl_session *s, rl_ticket t) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  s->state = RL_DRAINING;
  memset(&s->rx, 0, sizeof(s->rx)); /* incomplete RX is not an accepted send */
  return RL_OK;
}
rl_result rl_abort(rl_session *s, rl_ticket t) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if (s->state != RL_DRAINING) return RL_INVALID;
  s->in_call = true;
  /* ACK is only a local obligation, not an application-accepted TX. Explicit
   * shutdown abandonment must never publish an ACK success terminal. */
  s->ack.pending = false;
  if (s->tx.active) terminal(s, RL_TERMINAL_CANCELLED);
  s->in_call = false;
  return RL_OK;
}
rl_result rl_drain(rl_session *s, rl_ticket t) {
  rl_result st = validate(s, t);
  if (st != RL_OK) return st;
  if (s->state != RL_DRAINING) return RL_INVALID;
  return s->tx.active || s->ack.pending ? RL_BUSY : RL_OK;
}
rl_result rl_stop(rl_session *s, rl_ticket t) {
  rl_result st = rl_drain(s, t);
  if (st != RL_OK) return st;
  secure_wipe(s->config.tx_auth_key, RL_KEY_BYTES);
  secure_wipe(s->config.rx_auth_key, RL_KEY_BYTES);
  secure_wipe(s->tx.bytes, sizeof(s->tx.bytes));
  secure_wipe(s->rx.bytes, sizeof(s->rx.bytes));
  s->state = RL_STOPPED;
  return RL_OK;
}
