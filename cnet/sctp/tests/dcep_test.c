#include "sctp_dcep_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_owner { int admitted; } test_owner;
static int owned(void *v) { return ((test_owner *)v)->admitted; }

static void reliable_open_ack(void) {
  cnet_sctp_dcep client = {0}, server = {0};
  test_owner a = {1}, b = {1};
  uint8_t open[300], ack[300];
  static const uint8_t label[] = {'c','h','a','t'};
  static const uint8_t protocol[] = {'j','s','o','n'};
  size_t no = 0u, na = 0u;
  assert(cnet_sctp_dcep_init(&client, 8u, 1, owned, &a) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_init(&server, 8u, 0, owned, &b) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_open(&client, 0u, 0x00u, 256u, 0u,
      label, sizeof(label), protocol, sizeof(protocol),
      open, 11u, &no) == CNET_SCTP_DCEP_CAPACITY);
  assert(no == 0u && client.channels[0].state == 0u);
  assert(cnet_sctp_dcep_open(&client, 1u, 0x00u, 256u, 0u,
      label, sizeof(label), protocol, sizeof(protocol),
      open, sizeof(open), &no) == CNET_SCTP_DCEP_PROTOCOL);
  assert(cnet_sctp_dcep_open(&client, 0u, 0x00u, 256u, 0u,
      label, sizeof(label), protocol, sizeof(protocol),
      open, sizeof(open), &no) == CNET_SCTP_DCEP_OK);
  assert(no == 20u && open[0] == 3u && open[1] == 0u &&
         open[2] == 1u && open[3] == 0u);
  assert(open[8] == 0u && open[9] == sizeof(label));
  assert(open[10] == 0u && open[11] == sizeof(protocol));
  assert(client.channels[0].state == 1u && !cnet_sctp_dcep_ready(&client, 0u));
  assert(cnet_sctp_dcep_input(&server, 0u, open, no,
      ack, 0u, &na) == CNET_SCTP_DCEP_CAPACITY);
  assert(na == 0u && server.channels[0].state == 0u);
  assert(cnet_sctp_dcep_input(&server, 0u, open, no,
      ack, sizeof(ack), &na) == CNET_SCTP_DCEP_OK);
  assert(na == 1u && ack[0] == 2u);
  assert(!cnet_sctp_dcep_ready(&server, 0u));
  /* Simulate admission failure at the Owner boundary: no premature open. */
  b.admitted = 0;
  assert(cnet_sctp_dcep_ack_admitted(&server, 0u) == CNET_SCTP_DCEP_OWNER);
  assert(!cnet_sctp_dcep_ready(&server, 0u));
  b.admitted = 1;
  /* The caller retries admission of the already-built ACK bytes. */
  assert(cnet_sctp_dcep_ack_admitted(&server, 0u) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&server, 0u));
  assert(server.channels[0].label_size == sizeof(label));
  assert(memcmp(server.channels[0].label, label, sizeof(label)) == 0);
  assert(cnet_sctp_dcep_input(&client, 0u, ack, na,
      open, sizeof(open), &no) == CNET_SCTP_DCEP_OK);
  assert(no == 0u && cnet_sctp_dcep_ready(&client, 0u));
  assert(cnet_sctp_dcep_input(&server, 0u, open, 0u,
      ack, sizeof(ack), &na) == CNET_SCTP_DCEP_INVALID);
  assert(cnet_sctp_dcep_input(&server, 0u, ack, 1u,
      open, sizeof(open), &no) == CNET_SCTP_DCEP_PROTOCOL);
  assert(cnet_sctp_dcep_stop(&client) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_stop(&server) == CNET_SCTP_DCEP_OK);
  assert(!cnet_sctp_dcep_ready(&client, 0u));
}

static void unordered_and_negative(void) {
  cnet_sctp_dcep client = {0}, server = {0};
  test_owner a = {1}, b = {1};
  uint8_t open[300], ack[300];
  const uint8_t invalid_utf8[] = {0xc0u, 0x80u};
  const uint8_t valid_utf8[] = {0xe6u, 0xb5u, 0x8bu, 0xe8u, 0xafu, 0x95u};
  size_t n = 0u, na = 0u;
  assert(cnet_sctp_dcep_init(&client, 8u, 1, owned, &a) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_init(&server, 8u, 0, owned, &b) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_open(&server, 1u, 0x80u, 42u, 0u,
      valid_utf8, sizeof(valid_utf8), NULL, 0u,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_input(&client, 1u, open, n,
      ack, sizeof(ack), &na) == CNET_SCTP_DCEP_OK);
  assert(!cnet_sctp_dcep_ready(&client, 1u));
  assert(cnet_sctp_dcep_ack_admitted(&client, 1u) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&client, 1u));
  assert(client.channels[1].channel_type == 0x80u);
  assert(client.channels[1].label_size == sizeof(valid_utf8));
  assert(cnet_sctp_dcep_input(&server, 1u, ack, na,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&server, 1u));
  assert(cnet_sctp_dcep_open(&client, 2u, 0x01u, 0u, 2u,
      NULL, 0u, NULL, 0u,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_UNSUPPORTED);
  assert(cnet_sctp_dcep_open(&client, 2u, 0x00u, 0u, 0u,
      invalid_utf8, sizeof(invalid_utf8), NULL, 0u,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_PROTOCOL);
  assert(client.channels[2].state == 0u);
  assert(cnet_sctp_dcep_open(&client, 2u, 0x00u, 0u, 0u,
      NULL, 0u, NULL, 0u, open, sizeof(open), &n) == CNET_SCTP_DCEP_OK);
  /* Invalid ACK and remote OPEN on an occupied stream must not mutate. */
  ack[0] = 2u;
  assert(cnet_sctp_dcep_input(&server, 3u, ack, 1u,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_PROTOCOL);
  assert(cnet_sctp_dcep_input(&client, 2u, ack, 1u,
      open, sizeof(open), &n) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&client, 2u));
  a.admitted = 0;
  assert(cnet_sctp_dcep_open(&client, 4u, 0u, 0u, 0u,
      NULL, 0u, NULL, 0u, open, sizeof(open), &n) == CNET_SCTP_DCEP_OWNER);
  a.admitted = 1;
  assert(cnet_sctp_dcep_stop(&client) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_stop(&server) == CNET_SCTP_DCEP_OK);
}

static void output_length_on_early_errors(void) {
  cnet_sctp_dcep d = {0};
  test_owner owner = {1};
  uint8_t wire_ack = 2u, output[16];
  size_t written = 99u;
  assert(cnet_sctp_dcep_init(&d, 8u, 1, owned, &owner) == CNET_SCTP_DCEP_OK);

  /* Even failures before payload validation must not leak a stale length. */
  assert(cnet_sctp_dcep_open(&d, 0u, 0u, 0u, 0u, NULL, 0u, NULL, 0u,
      NULL, sizeof(output), &written) == CNET_SCTP_DCEP_INVALID);
  assert(written == 0u);

  written = 99u;
  owner.admitted = 0;
  assert(cnet_sctp_dcep_open(&d, 0u, 0u, 0u, 0u, NULL, 0u, NULL, 0u,
      output, sizeof(output), &written) == CNET_SCTP_DCEP_OWNER);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_dcep_input(&d, 0u, &wire_ack, 1u,
      output, sizeof(output), &written) == CNET_SCTP_DCEP_OWNER);
  assert(written == 0u);

  owner.admitted = 1;
  assert(cnet_sctp_dcep_stop(&d) == CNET_SCTP_DCEP_OK);
  written = 99u;
  assert(cnet_sctp_dcep_open(&d, 0u, 0u, 0u, 0u, NULL, 0u, NULL, 0u,
      output, sizeof(output), &written) == CNET_SCTP_DCEP_CLOSED);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_dcep_input(&d, 0u, &wire_ack, 1u,
      output, sizeof(output), &written) == CNET_SCTP_DCEP_CLOSED);
  assert(written == 0u);
}

int main(void) {
  reliable_open_ack();
  output_length_on_early_errors();
  unordered_and_negative();
  puts("C11 RFC8832 reliable DCEP OPEN/ACK, UTF-8, parity, bounds PASS");
  return 0;
}
