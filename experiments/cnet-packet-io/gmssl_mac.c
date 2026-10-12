#include "gmssl_mac.h"

#include <gmssl/digest.h>
#include <gmssl/hmac.h>
#include <gmssl/mem.h>

#include <string.h>

static bool lease_valid(const rl_gmssl_lease *lease) {
  const rl_session *s;
  if (!lease || !lease->bound || !lease->session ||
      !pthread_equal(lease->owner, pthread_self())) return false;
  s = lease->session;
  return (s->state == RL_OPEN || s->state == RL_DRAINING) &&
         s->generation == lease->generation &&
         s->config.epoch == lease->epoch &&
         s->config.local_id == lease->local_id &&
         s->config.peer_id == lease->peer_id &&
         s->callbacks.mac == rl_gmssl_mac && s->callbacks.mac_user == lease;
}

rl_gmssl_status rl_gmssl_lease_bind(rl_gmssl_lease *lease, rl_session *s,
                                   rl_ticket ticket) {
  if (!lease || !s || !ticket.generation ||
      s->state != RL_OPEN || s->generation != ticket.generation ||
      !s->config.epoch || !s->config.peer_id || !s->config.local_id ||
      s->callbacks.mac != rl_gmssl_mac || s->callbacks.mac_user != lease)
    return RL_GMSSL_INVALID;
  if (!pthread_equal(s->owner, pthread_self())) return RL_GMSSL_WRONG_OWNER;
  if (lease->bound || lease->in_mac) return RL_GMSSL_BUSY;
  if (lease->generation >= ticket.generation) return RL_GMSSL_STALE;
  lease->session = s;
  lease->owner = pthread_self();
  lease->generation = ticket.generation;
  lease->epoch = s->config.epoch;
  lease->local_id = s->config.local_id;
  lease->peer_id = s->config.peer_id;
  lease->bound = true;
  lease->sealed = false;
  return RL_GMSSL_OK;
}

rl_gmssl_status rl_gmssl_lease_seal(rl_gmssl_lease *lease, rl_ticket ticket) {
  if (!lease || !lease->bound) return RL_GMSSL_INVALID;
  if (!pthread_equal(lease->owner, pthread_self())) return RL_GMSSL_WRONG_OWNER;
  if (ticket.generation != lease->generation) return RL_GMSSL_STALE;
  if (lease->in_mac || !lease->session || lease->session->state != RL_DRAINING)
    return RL_GMSSL_BUSY;
  lease->sealed = true;
  return RL_GMSSL_OK;
}

rl_gmssl_status rl_gmssl_lease_release(rl_gmssl_lease *lease, rl_ticket ticket) {
  if (!lease || !lease->bound) return RL_GMSSL_INVALID;
  if (!pthread_equal(lease->owner, pthread_self())) return RL_GMSSL_WRONG_OWNER;
  if (ticket.generation != lease->generation) return RL_GMSSL_STALE;
  if (!lease->sealed || lease->in_mac || !lease->session ||
      lease->session->state != RL_STOPPED) return RL_GMSSL_BUSY;
  lease->session = NULL;
  lease->epoch = 0;
  lease->local_id = 0;
  lease->peer_id = 0;
  lease->bound = false;
  lease->sealed = false;
  /* Keep the generation watermark to prevent an accidental stale rebind. */
  return RL_GMSSL_OK;
}

bool rl_gmssl_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  rl_gmssl_lease *lease = (rl_gmssl_lease *)mac_user;
  uint8_t temporary[RL_MAC_BYTES] = {0};
  size_t mac_length = 0;
  bool ok = false;
  if (!key || !bytes || !out || size < RL_WIRE_HEADER_BYTES ||
      size > RL_WIRE_HEADER_BYTES + RL_FRAGMENT_BYTES ||
      !lease_valid(lease) || lease->in_mac != 0) return false;
  ++lease->in_mac;
  if (hmac(DIGEST_sha256(), key, RL_KEY_BYTES,
           (const uint8_t *)bytes, size, temporary, &mac_length) == 1 &&
      mac_length == RL_MAC_BYTES) {
    memcpy(out, temporary, RL_MAC_BYTES);
    ok = true;
  }
  gmssl_secure_clear(temporary, sizeof(temporary));
  --lease->in_mac;
  return ok;
}
