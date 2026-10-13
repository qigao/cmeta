#include "sctp_init_params_internal.h"

#ifdef NDEBUG
#undef NDEBUG /* Keep actual safety tests enabled in Release CI. */
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t x) {
  p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
}

static size_t append(uint8_t *p, size_t pos, uint16_t kind, size_t value_len) {
  const size_t len = value_len + 4u;
  const size_t padded = (len + 3u) & ~(size_t)3u;
  assert(padded <= 512u - pos);
  assert(len <= UINT16_MAX);
  put16(p + pos, kind);
  put16(p + pos + 2u, (uint16_t)len);
  memset(p + pos + 4u, 0x51, value_len);
  memset(p + pos + len, 0, padded - len);
  return pos + padded;
}

static void assert_failure_does_not_expose_borrowed_cookie(const uint8_t *p,
    size_t n, int ack, cnet_sctp_init_param_status expected) {
  cnet_sctp_init_param_view view = {0};
  view.cookie = (const uint8_t *)"old-view";
  view.cookie_size = 123;
  assert(cnet_sctp_init_params_preflight(p, n, ack, &view) == expected);
  assert(view.cookie == NULL && view.cookie_size == 0u);
}

static void test_cookie_and_padding(void) {
  uint8_t p[512] = {0};
  cnet_sctp_init_param_view out = {0};
  size_t n = 16u;
  assert(cnet_sctp_init_params_preflight(p, n, 0, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.cookie == NULL && out.parsed_count == 0u);
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  n = append(p, n, 7u, 3u); /* Cookie len 7 + final padding 1 */
  assert(cnet_sctp_init_params_preflight(p, n, 1, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.cookie == p + 20u && out.cookie_size == 3u && out.parsed_count == 1u);
  assert(cnet_sctp_init_params_preflight(p, n - 1u, 1, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.cookie_size == 3u); /* final padding is optional */
  n = append(p, n, 0x8005u, 1u); /* unknown 10 => skip, no report */
  assert(cnet_sctp_init_params_preflight(p, n, 1, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.cookie_size == 3u && out.skipped_count == 1u && out.parsed_count == 2u);
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_MALFORMED);
  n = append(p, n, 7u, 2u);
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
}

static void test_unknown_actions(void) {
  uint8_t p[512] = {0};
  cnet_sctp_init_param_view out = {0};
  size_t n = 16u;
  n = append(p, n, 0x8005u, 0u);
  n = append(p, n, 0x0002u, 0u); /* unknown 00: stop parsing */
  n = append(p, n, 0x4003u, 0u); /* must never inspect/report after stop */
  assert(cnet_sctp_init_params_preflight(p, n, 0, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.stopped_by_unknown == 1 && out.skipped_count == 1u && out.parsed_count == 2u);
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  put16(p + 20u, 0x4002u); /* unknown 01: report required, not implemented */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_REPORT_REQUIRED);
  put16(p + 20u, 0xc002u); /* unknown 11: skip + report, not implemented */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_REPORT_REQUIRED);
  put16(p + 20u, 0x8005u);
  /* Unknown 00 after mandatory State Cookie permits COOKIE ECHO. */
  memset(p, 0, sizeof(p));
  n = append(p, 16u, 7u, 1u);
  n = append(p, n, 0x0002u, 0u);
  n = append(p, n, 0x4003u, 0u);
  assert(cnet_sctp_init_params_preflight(p, n, 1, &out) == CNET_SCTP_INIT_PARAMS_OK);
  assert(out.cookie != NULL && out.cookie_size == 1u && out.stopped_by_unknown == 1);
}

static void test_malformed_and_unsupported(void) {
  uint8_t p[512] = {0};
  size_t n;
  n = append(p, 16u, 7u, 0u); /* required state cookie cannot be empty */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  p[19u] = 0u; /* zero parameter length */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  p[19u] = 80u; /* greater than available */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  n = append(p, 16u, 5u, 4u); /* IPv4 recognized, but no path handling yet */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  p[19u] = 7u;
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_MALFORMED);
  n = append(p, 16u, 6u, 16u); /* IPv6 */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 9u, 4u); /* Cookie Preservative, not implemented */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 11u, 4u); /* prohibited hostname */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 12u, 2u); /* Supported Address Types */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 0x8000u, 0u); /* ECN, not implemented */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 8u, 4u); /* Unrecognized Parameter only legal on INIT ACK */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_MALFORMED);
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_UNSUPPORTED);
  n = append(p, 16u, 7u, 4u);
  p[n++] = 0u; /* trailing 1 byte cannot encode next parameter */
  assert_failure_does_not_expose_borrowed_cookie(p, n, 1, CNET_SCTP_INIT_PARAMS_MALFORMED);
  assert(cnet_sctp_init_params_preflight(NULL, 20u, 1, NULL) == CNET_SCTP_INIT_PARAMS_INVALID);
}

static void test_capacity_and_mutations(void) {
  uint8_t p[640] = {0};
  cnet_sctp_init_param_view out;
  size_t n = 16u;
  uint32_t seed = UINT32_C(0x4e92ab45);
  size_t i;
  for (i = 0u; i < CNET_SCTP_INIT_TLV_CEILING + 1u; ++i) {
    put16(p + n, 0x8005u);
    put16(p + n + 2u, 4u);
    n += 4u;
  }
  assert_failure_does_not_expose_borrowed_cookie(p, n, 0, CNET_SCTP_INIT_PARAMS_CAPACITY);
  for (i = 0u; i < 10000u; ++i) {
    size_t length;
    size_t j;
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    length = 16u + (size_t)(seed % 400u);
    for (j = 16u; j < length; ++j) {
      seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
      p[j] = (uint8_t)(seed >> 24);
    }
    for (j = 0u; j < 2u; ++j) {
      cnet_sctp_init_param_status result = cnet_sctp_init_params_preflight(
          p, length, (int)j, &out);
      assert(result <= CNET_SCTP_INIT_PARAMS_OK &&
             result >= CNET_SCTP_INIT_PARAMS_CAPACITY);
      if (result == CNET_SCTP_INIT_PARAMS_OK && out.cookie) {
        assert(out.cookie >= p + 16u && out.cookie <= p + length);
        assert(out.cookie_size <= (size_t)(p + length - out.cookie));
      }
    }
  }
}

int main(void) {
  test_cookie_and_padding();
  test_unknown_actions();
  test_malformed_and_unsupported();
  test_capacity_and_mutations();
  puts("C11 SCTP RFC9260 INIT TLV restrictive preflight: PASS (no ERROR/ABORT yet)");
  return 0;
}
