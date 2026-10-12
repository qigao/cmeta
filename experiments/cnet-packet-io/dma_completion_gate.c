#include "dma_completion_gate.h"

#include <limits.h>
#include <string.h>

static r2_dma_status check_owner(r2_dma_gate *g) {
  if (!g || !g->initialized || !g->running) return R2_DMA_INVALID;
  if (!pthread_equal(g->owner, pthread_self())) return R2_DMA_WRONG_OWNER;
  if (g->polling) return R2_DMA_BUSY;
  return R2_DMA_OK;
}
static r2_dma_slot *find_ticket(r2_dma_gate *g, r2_dma_ticket ticket) {
  if (!ticket.generation || !ticket.serial ||
      ticket.generation != g->generation || ticket.slot >= g->capacity)
    return NULL;
  r2_dma_slot *s = &g->slots[ticket.slot];
  return s->serial == ticket.serial ? s : NULL;
}

r2_dma_status r2_dma_gate_start(r2_dma_gate *g, r2_mux *mux,
                                uint64_t generation, size_t capacity) {
  if (!g || !mux || !mux->running ||
      !pthread_equal(mux->owner, pthread_self()) ||
      !generation || !capacity || capacity > R2_DMA_GATE_MAX_TICKETS)
    return R2_DMA_INVALID;
  if (!g->initialized) {
    /* An initially zeroed, caller-owned gate is required. */
    if (pthread_mutex_init(&g->mutex, NULL) != 0) return R2_DMA_OWNER_ERROR;
    g->initialized = true;
  }
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  r2_dma_status st = R2_DMA_OK;
  if (g->running || g->active || g->posted) st = R2_DMA_BUSY;
  else if (generation <= g->generation) st = R2_DMA_STALE;
  else if (g->generation && !g->producers_fenced) st = R2_DMA_BUSY;
  else {
    g->owner = pthread_self();
    g->mux = mux;
    g->generation = generation;
    g->capacity = capacity;
    g->active = 0;
    g->posted = 0;
    g->polling = false;
    g->sealed = false;
    g->producers_fenced = false;
    g->running = true;
  }
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_arm(r2_dma_gate *g, r2_mux_handle handle,
                             packet_io_release_fn finalizer, void *context,
                             uint64_t finalizer_tag, r2_dma_ticket *out) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (!finalizer || !out) return R2_DMA_INVALID;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  if (g->sealed || g->producers_fenced) st = R2_DMA_SEALED;
  else if (g->active == g->capacity) st = R2_DMA_FULL;
  else if (g->next_serial == UINT64_MAX) st = R2_DMA_STALE;
  else {
    size_t index = g->capacity;
    for (size_t i = 0; i < g->capacity; ++i)
      if (g->slots[i].state == R2_DMA_FREE) { index = i; break; }
    if (index == g->capacity) st = R2_DMA_FULL;
    else {
      r2_mux_status pin = r2_mux_frame_pin(g->mux, handle);
      if (pin == R2_MUX_FULL) st = R2_DMA_FULL;
      else if (pin == R2_MUX_STALE) st = R2_DMA_STALE;
      else if (pin != R2_MUX_OK) st = R2_DMA_OWNER_ERROR;
      else {
        r2_dma_slot *slot = &g->slots[index];
        slot->handle = handle;
        slot->finalizer = finalizer;
        slot->finalizer_context = context;
        slot->finalizer_tag = finalizer_tag;
        slot->serial = ++g->next_serial;
        slot->state = R2_DMA_ARMED;
        ++g->active;
        out->generation = g->generation;
        out->serial = slot->serial;
        out->slot = (uint8_t)index;
      }
    }
  }
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_rollback(r2_dma_gate *g, r2_dma_ticket t) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  r2_dma_slot *slot = find_ticket(g, t);
  if (!slot || slot->state == R2_DMA_FREE) st = R2_DMA_STALE;
  else if (slot->state != R2_DMA_ARMED) st = R2_DMA_BUSY;
  else if (r2_mux_frame_unpin(g->mux, slot->handle) != R2_MUX_OK)
    st = R2_DMA_OWNER_ERROR;
  else {
    slot->state = R2_DMA_FREE;
    slot->finalizer = NULL;
    slot->finalizer_context = NULL;
    --g->active;
  }
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_post(r2_dma_gate *g, r2_dma_ticket t) {
  if (!g || !g->initialized) return R2_DMA_INVALID;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  r2_dma_status st = R2_DMA_OK;
  if (!g->running || !t.generation || t.generation != g->generation)
    st = R2_DMA_STALE;
  else if (g->producers_fenced) st = R2_DMA_SEALED;
  else {
    r2_dma_slot *slot = find_ticket(g, t);
    if (!slot || slot->state == R2_DMA_FREE) st = R2_DMA_STALE;
    else if (slot->state == R2_DMA_POSTED || slot->state == R2_DMA_RECLAIMING)
      st = R2_DMA_DUPLICATE;
    else {
      slot->state = R2_DMA_POSTED;
      ++g->posted;
    }
  }
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_poll(r2_dma_gate *g, size_t budget,
                               size_t *out_finalized) {
  if (!out_finalized) return R2_DMA_INVALID;
  *out_finalized = 0;
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (budget > R2_DMA_GATE_MAX_TICKETS) return R2_DMA_INVALID;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  g->polling = true;
  while (*out_finalized < budget && g->posted) {
    size_t index = g->capacity;
    for (size_t i = 0; i < g->capacity; ++i)
      if (g->slots[i].state == R2_DMA_POSTED) { index = i; break; }
    if (index == g->capacity) { st = R2_DMA_OWNER_ERROR; break; }
    r2_dma_slot *slot = &g->slots[index];
    r2_mux_handle handle = slot->handle;
    packet_io_release_fn finalize = slot->finalizer;
    void *context = slot->finalizer_context;
    uint64_t tag = slot->finalizer_tag;
    slot->state = R2_DMA_RECLAIMING; /* exactly one consumer may claim it */
    --g->posted;
    pthread_mutex_unlock(&g->mutex);

    /* The only authoritative physical reference release. An external thread
     * may post an observation, but must never call this owner operation. */
    if (r2_mux_frame_unpin(g->mux, handle) != R2_MUX_OK) {
      st = R2_DMA_OWNER_ERROR; /* leave RECLAIMING as a visible blocking fault */
      pthread_mutex_lock(&g->mutex);
      break;
    }
    finalize(context, tag); /* same Owner, once; never implies remote ACK */
    ++*out_finalized;

    pthread_mutex_lock(&g->mutex);
    slot->state = R2_DMA_FREE;
    slot->finalizer = NULL;
    slot->finalizer_context = NULL;
    --g->active;
  }
  g->polling = false;
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_seal(r2_dma_gate *g) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  g->sealed = true;
  pthread_mutex_unlock(&g->mutex);
  return R2_DMA_OK;
}

r2_dma_status r2_dma_gate_fence_joined_producers(r2_dma_gate *g) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  if (!g->sealed) st = R2_DMA_INVALID;
  else g->producers_fenced = true;
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_drain(r2_dma_gate *g) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  if (!g->sealed) st = R2_DMA_INVALID;
  else if (g->active || g->posted) st = R2_DMA_BUSY;
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_stop(r2_dma_gate *g) {
  r2_dma_status st = check_owner(g);
  if (st != R2_DMA_OK) return st;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  if (!g->sealed || !g->producers_fenced || g->active || g->posted)
    st = R2_DMA_BUSY;
  else {
    g->running = false;
    g->mux = NULL;
  }
  pthread_mutex_unlock(&g->mutex);
  return st;
}

r2_dma_status r2_dma_gate_destroy(r2_dma_gate *g) {
  if (!g || !g->initialized) return R2_DMA_INVALID;
  if (!pthread_equal(g->owner, pthread_self())) return R2_DMA_WRONG_OWNER;
  if (pthread_mutex_lock(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  if (g->running || !g->producers_fenced || g->active || g->posted) {
    pthread_mutex_unlock(&g->mutex);
    return R2_DMA_BUSY;
  }
  pthread_mutex_unlock(&g->mutex);
  if (pthread_mutex_destroy(&g->mutex) != 0) return R2_DMA_OWNER_ERROR;
  g->initialized = false;
  return R2_DMA_OK;
}
