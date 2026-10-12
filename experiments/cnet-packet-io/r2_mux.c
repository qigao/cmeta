#include "r2_mux.h"

#include <limits.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static r2_mux_status owner_check(r2_mux *m) {
  if (!m || !m->running) return R2_MUX_INVALID;
  if (!pthread_equal(m->owner, pthread_self())) return R2_MUX_WRONG_OWNER;
  if (m->in_call) return R2_MUX_BUSY;
  return R2_MUX_OK;
}

static r2_mux_status checked_slot(r2_mux *m, r2_mux_handle h,
                                  r2_mux_slot **out) {
  if (!m || !m->running) return R2_MUX_INVALID;
  if (!pthread_equal(m->owner, pthread_self())) return R2_MUX_WRONG_OWNER;
  if (h.slot >= m->capacity) return R2_MUX_INVALID;
  r2_mux_slot *slot = &m->slots[h.slot];
  if (!slot->active || slot->generation != h.generation) return R2_MUX_STALE;
  *out = slot;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_start(r2_mux *m, uint64_t generation,
                           uint8_t capacity, uint8_t per_session_frame_limit) {
  if (!m || !generation || !capacity || capacity > R2_MUX_MAX_SESSIONS ||
      !per_session_frame_limit ||
      per_session_frame_limit > R2_MUX_MAX_PHYSICAL_REFS)
    return R2_MUX_INVALID;
  if (m->running) return R2_MUX_BUSY;
  if (generation <= m->generation) return R2_MUX_STALE;
  for (unsigned i = 0; i < R2_MUX_MAX_SESSIONS; ++i)
    if (m->slots[i].active || m->slots[i].physical_refs)
      return R2_MUX_BUSY;
  m->owner = pthread_self();
  m->generation = generation;
  m->now_ms = 0;
  m->child_errors = 0;
  m->unroutable = 0;
  m->cursor = 0;
  m->capacity = capacity;
  m->frame_limit = per_session_frame_limit;
  m->active_count = 0;
  m->sealed = false;
  m->in_call = false;
  for (unsigned i = 0; i < R2_MUX_MAX_SESSIONS; ++i) {
    r2_mux_slot *slot = &m->slots[i];
    slot->session = NULL;
    /* Preserve the last identity as a tombstone across Owner restarts. */
    slot->ticket.generation = 0;
    slot->last_error = RL_OK;
  }
  m->running = true;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_attach(r2_mux *m, rl_session *s, rl_ticket ticket,
                            r2_mux_handle *out) {
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  if (m->sealed) return R2_MUX_SEALED;
  if (!s || !out || !ticket.generation ||
      s->state != RL_OPEN || s->generation != ticket.generation ||
      !s->config.local_id || !s->config.peer_id || !s->config.epoch ||
      !s->callbacks.mac || !s->callbacks.mac_user)
    return R2_MUX_INVALID;
  if (!pthread_equal(s->owner, pthread_self())) return R2_MUX_WRONG_OWNER;
  if (m->active_count == m->capacity) return R2_MUX_FULL;
  for (unsigned i = 0; i < m->capacity; ++i) {
    const r2_mux_slot *other = &m->slots[i];
    if (other->active && other->session == s) return R2_MUX_INVALID;
    if (other->generation && other->local_id == s->config.local_id &&
        other->peer_id == s->config.peer_id &&
        other->epoch == s->config.epoch)
      return other->active ? R2_MUX_INVALID : R2_MUX_STALE;
  }
  for (unsigned i = 0; i < m->capacity; ++i) {
    r2_mux_slot *slot = &m->slots[i];
    if (slot->active) continue;
    if (slot->generation == UINT64_MAX) return R2_MUX_STALE;
    ++slot->generation;
    slot->active = true;
    slot->session = s;
    slot->ticket = ticket;
    slot->local_id = s->config.local_id;
    slot->peer_id = s->config.peer_id;
    slot->epoch = s->config.epoch;
    slot->physical_refs = 0;
    slot->last_error = RL_OK;
    out->slot = (uint8_t)i;
    out->generation = slot->generation;
    ++m->active_count;
    return R2_MUX_OK;
  }
  return R2_MUX_FULL;
}

r2_mux_status r2_mux_detach(r2_mux *m, r2_mux_handle handle) {
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  r2_mux_slot *slot = NULL;
  st = checked_slot(m, handle, &slot);
  if (st != R2_MUX_OK) return st;
  if (!slot->session || slot->session->generation != slot->ticket.generation)
    return R2_MUX_STALE;
  if (slot->physical_refs || slot->session->state != RL_STOPPED) return R2_MUX_BUSY;
  slot->session = NULL;
  slot->ticket.generation = 0;
  /* Keep last peer/epoch tuple, refuse immediate epoch replay on reuse. */
  slot->active = false;
  --m->active_count;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_frame_pin(r2_mux *m, r2_mux_handle handle) {
  r2_mux_slot *slot = NULL;
  r2_mux_status st = checked_slot(m, handle, &slot);
  if (st != R2_MUX_OK) return st;
  if (!slot->session || slot->session->state == RL_STOPPED)
    return R2_MUX_SEALED;
  if (slot->physical_refs == m->frame_limit) return R2_MUX_FULL;
  ++slot->physical_refs;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_frame_unpin(r2_mux *m, r2_mux_handle handle) {
  r2_mux_slot *slot = NULL;
  r2_mux_status st = checked_slot(m, handle, &slot);
  if (st != R2_MUX_OK) return st;
  if (!slot->physical_refs) return R2_MUX_INVALID;
  --slot->physical_refs;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_step(r2_mux *m, uint64_t now_ms, size_t budget,
                          size_t *out_emitted) {
  if (!out_emitted) return R2_MUX_INVALID;
  *out_emitted = 0;
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  if (budget > R2_MUX_MAX_BUDGET || now_ms < m->now_ms) return R2_MUX_INVALID;
  m->now_ms = now_ms;
  if (!budget || !m->active_count) return R2_MUX_OK;
  bool child_error = false;
  size_t idle_visits = 0;
  m->in_call = true;
  while (*out_emitted < budget && idle_visits < m->capacity) {
    unsigned i = m->cursor;
    m->cursor = (uint8_t)((i + 1u) % m->capacity);
    r2_mux_slot *slot = &m->slots[i];
    if (!slot->active) { ++idle_visits; continue; }
    size_t emitted = 0;
    rl_result child = rl_step(slot->session, slot->ticket, now_ms, 1u, &emitted);
    if (child != RL_OK) {
      slot->last_error = child;
      ++m->child_errors;
      child_error = true;
      ++idle_visits;
    } else if (emitted == 1u) {
      ++*out_emitted;
      idle_visits = 0;
    } else {
      ++idle_visits;
    }
  }
  m->in_call = false;
  return child_error ? R2_MUX_CHILD_ERROR : R2_MUX_OK;
}

r2_mux_status r2_mux_input(r2_mux *m, const void *wire, size_t size,
                           uint64_t now_ms, rl_result *out_child) {
  if (!out_child) return R2_MUX_INVALID;
  *out_child = RL_INVALID;
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  if (!wire || size < RL_WIRE_HEADER_BYTES + RL_MAC_BYTES ||
      size > RL_MAX_WIRE_BYTES || now_ms < m->now_ms) return R2_MUX_INVALID;
  m->now_ms = now_ms;
  const uint8_t *p = (const uint8_t *)wire;
  if (memcmp(p, "SRE2", 4) || p[4] != 2u) {
    ++m->unroutable;
    return R2_MUX_UNROUTABLE;
  }
  uint32_t src = read32(p + 8), dst = read32(p + 12), epoch = read32(p + 16);
  for (unsigned i = 0; i < m->capacity; ++i) {
    r2_mux_slot *slot = &m->slots[i];
    if (!slot->active || slot->local_id != dst ||
        slot->peer_id != src || slot->epoch != epoch) continue;
    m->in_call = true;
    *out_child = rl_input(slot->session, slot->ticket, wire, size, now_ms);
    m->in_call = false;
    if (*out_child != RL_OK) {
      slot->last_error = *out_child;
      ++m->child_errors;
    }
    return R2_MUX_OK; /* caller inspects authenticated child result */
  }
  ++m->unroutable;
  return R2_MUX_UNROUTABLE;
}

r2_mux_status r2_mux_seal(r2_mux *m) {
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  m->sealed = true;
  return R2_MUX_OK;
}

r2_mux_status r2_mux_drain(r2_mux *m) {
  r2_mux_status st = owner_check(m);
  if (st != R2_MUX_OK) return st;
  if (!m->sealed) return R2_MUX_INVALID;
  return m->active_count ? R2_MUX_BUSY : R2_MUX_OK;
}

r2_mux_status r2_mux_stop(r2_mux *m) {
  r2_mux_status st = r2_mux_drain(m);
  if (st != R2_MUX_OK) return st;
  m->running = false;
  return R2_MUX_OK;
}
