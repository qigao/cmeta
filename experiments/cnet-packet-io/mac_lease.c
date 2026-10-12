#include "mac_lease.h"

#include <string.h>

rl_mac_status rl_mac_lease_bind(rl_mac_lease *lease, rl_session *s,
                                rl_ticket ticket) {
  if (!lease || !s || !ticket.generation ||
      s->state != RL_OPEN || s->generation != ticket.generation ||
      !s->config.epoch || !s->config.peer_id || !s->config.local_id ||
      !s->callbacks.mac || s->callbacks.mac_user != lease)
    return RL_MAC_INVALID;
  if (!pthread_equal(s->owner, pthread_self())) return RL_MAC_WRONG_OWNER;
  if (lease->bound || lease->in_mac) return RL_MAC_BUSY;
  if (lease->generation >= ticket.generation) return RL_MAC_STALE;
  lease->session = s;
  lease->owner = pthread_self();
  lease->generation = ticket.generation;
  lease->epoch = s->config.epoch;
  lease->local_id = s->config.local_id;
  lease->peer_id = s->config.peer_id;
  lease->provider = s->callbacks.mac;
  lease->bound = true;
  lease->sealed = false;
  return RL_MAC_OK;
}

rl_mac_status rl_mac_lease_seal(rl_mac_lease *lease, rl_ticket ticket) {
  if (!lease || !lease->bound) return RL_MAC_INVALID;
  if (!pthread_equal(lease->owner, pthread_self())) return RL_MAC_WRONG_OWNER;
  if (ticket.generation != lease->generation) return RL_MAC_STALE;
  if (lease->in_mac || !lease->session || lease->session->state != RL_DRAINING)
    return RL_MAC_BUSY;
  lease->sealed = true;
  return RL_MAC_OK;
}

rl_mac_status rl_mac_lease_release(rl_mac_lease *lease, rl_ticket ticket) {
  if (!lease || !lease->bound) return RL_MAC_INVALID;
  if (!pthread_equal(lease->owner, pthread_self())) return RL_MAC_WRONG_OWNER;
  if (ticket.generation != lease->generation) return RL_MAC_STALE;
  if (!lease->sealed || lease->in_mac || !lease->session ||
      lease->session->state != RL_STOPPED) return RL_MAC_BUSY;
  lease->session = NULL;
  lease->epoch = 0;
  lease->local_id = 0;
  lease->peer_id = 0;
  lease->provider = NULL;
  lease->bound = false;
  lease->sealed = false;
  /* Preserve the monotonic generation watermark across N -> N+1. */
  return RL_MAC_OK;
}

bool rl_mac_lease_enter(rl_mac_lease *lease, rl_mac_fn requested) {
  const rl_session *s;
  if (!lease || !lease->bound || !lease->session || !requested ||
      !pthread_equal(lease->owner, pthread_self()) || lease->in_mac != 0)
    return false;
  s = lease->session;
  if ((s->state != RL_OPEN && s->state != RL_DRAINING) ||
      s->generation != lease->generation ||
      s->config.epoch != lease->epoch ||
      s->config.local_id != lease->local_id ||
      s->config.peer_id != lease->peer_id ||
      s->callbacks.mac_user != lease ||
      s->callbacks.mac != requested || lease->provider != requested)
    return false;
  ++lease->in_mac;
  return true;
}

void rl_mac_lease_leave(rl_mac_lease *lease) {
  if (lease && lease->in_mac && pthread_equal(lease->owner, pthread_self()))
    --lease->in_mac;
}
