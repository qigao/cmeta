#include "cmeta_error.h"
#include "tinytest.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define TEST_ERROR_DOMAIN 42
#define TEST_ERROR_AUTH SALTS_ERROR_CUSTOM(TEST_ERROR_DOMAIN, 1)
#define TEST_ERROR_PARSE SALTS_ERROR_CUSTOM(TEST_ERROR_DOMAIN, 2)

static const cmeta_error_entry_t test_errors[] = {
    {TEST_ERROR_AUTH, "TEST_EAUTH", "test authentication failed"},
    {TEST_ERROR_PARSE, "TEST_EPARSE", "test parse failed"},
};

static const cmeta_error_domain_desc_t test_domain = {
    TEST_ERROR_DOMAIN,
    "test",
    test_errors,
    sizeof(test_errors) / sizeof(test_errors[0]),
};

suite("Salts Error") {
  group("Salts codes") {
    it("describes common and protocol errors") {
      check_equal(cmeta_strerror(SALTS_OK), "success");
      check_equal(cmeta_strerror(SALTS_EINVAL), "invalid argument");
      check_equal(cmeta_strerror(SALTS_EPROTO), "protocol error");
      check_equal(cmeta_strerror(SALTS_EAI_FAMILY), "address family not supported by DNS result");
    }

    it("describes authentication failures as a stable Salts code") {
      cmeta_error_info_t info = cmeta_error_info(SALTS_EAUTH);

      check_equal(SALTS_EAUTH, -4054);
      check_equal(cmeta_strerror(SALTS_EAUTH), "authentication failed");
      check_equal(info.code, SALTS_EAUTH);
      check_equal(info.domain, SALTS_ERROR_DOMAIN_SALTS);
      check_equal(info.name, "SALTS_EAUTH");
      check_equal(info.message, "authentication failed");
    }

    it("returns structured metadata") {
      cmeta_error_info_t info = cmeta_error_info(SALTS_ENOMEM);

      check_equal(info.code, SALTS_ENOMEM);
      check_equal(info.domain, SALTS_ERROR_DOMAIN_SALTS);
      check_equal(info.name, "SALTS_ENOMEM");
      check_equal(info.message, "not enough memory");
    }

    it("maps cmeta_error_domain enum to and from string") {
      cmeta_error_domain_t domain = SALTS_ERROR_DOMAIN_UNKNOWN;
      const cmeta_enum_desc *meta = cmeta_error_domain_t_meta();

      check_not_null(meta);
      check_equal(meta->name, "cmeta_error_domain_t");
      check_equal(meta->count, (size_t)6u);
      check_equal(cmeta_error_domain_t_to_symbol(SALTS_ERROR_DOMAIN_WIN32),
                  "SALTS_ERROR_DOMAIN_WIN32");
      check_equal(cmeta_error_domain_t_to_string(SALTS_ERROR_DOMAIN_WIN32), "win32");
      check_true(cmeta_error_domain_t_from_string("SALTS_ERROR_DOMAIN_WIN32", &domain));
      check_equal(domain, SALTS_ERROR_DOMAIN_WIN32);
      check_equal(cmeta_error_domain_to_string(SALTS_ERROR_DOMAIN_NONE), "none");
      check_equal(cmeta_error_domain_to_string(SALTS_ERROR_DOMAIN_CUSTOM), "custom");
      check_equal(cmeta_error_domain_from_string("win32", &domain), 0);
      check_equal(domain, SALTS_ERROR_DOMAIN_WIN32);
      domain = SALTS_ERROR_DOMAIN_POSIX;
      check_equal(cmeta_error_domain_from_string("SALTS_ERROR_DOMAIN_WIN32", &domain), -1);
      check_equal(domain, SALTS_ERROR_DOMAIN_POSIX);
      check_equal(cmeta_error_domain_from_string("not_found", &domain), -1);
      check_equal(cmeta_error_domain_to_string((cmeta_error_domain_t)999), "unknown");
      check_equal(cmeta_error_domain_count(), (size_t)6u);
      check_true(cmeta_error_domain_is_valid(SALTS_ERROR_DOMAIN_SALTS));
      check_false(cmeta_error_domain_is_valid((cmeta_error_domain_t)999));
      check_true(cmeta_error_domain_equals(SALTS_ERROR_DOMAIN_SALTS,
                                           SALTS_ERROR_DOMAIN_SALTS));
    }
  }

  group("Native codes") {
    it("describes negative errno values") {
      cmeta_error_info_t info = cmeta_error_info(-ENOENT);

      check_equal(info.domain, SALTS_ERROR_DOMAIN_POSIX);
      check_equal(info.name, "POSIX");
      check_not_null(info.message);
      check(strstr(info.message, "unknown") == NULL && strstr(info.message, "Unknown") == NULL);
    }

#ifdef _WIN32
    it("describes negative Win32 values") {
      cmeta_error_info_t info = cmeta_error_info(-(int)ERROR_PIPE_BUSY);

      check_equal(info.domain, SALTS_ERROR_DOMAIN_WIN32);
      check_equal(info.name, "WIN32");
      check_not_null(info.message);
      check(strstr(info.message, "unknown") == NULL && strstr(info.message, "Unknown") == NULL);
    }
#endif
  }

  group("Result wrapper") {
    it("wraps success and errors") {
      cmeta_result_t ok = cmeta_result_from_code(SALTS_OK);
      cmeta_result_t err = cmeta_result_from_code(SALTS_EPERM);

      check_true(cmeta_result_is_ok(ok));
      check_false(cmeta_result_is_err(ok));
      check_equal(ok.code, SALTS_OK);
      check_equal(ok.message, "success");

      check_false(cmeta_result_is_ok(err));
      check_true(cmeta_result_is_err(err));
      check_equal(err.code, SALTS_EPERM);
      check_equal(err.message, "operation not permitted");
    }
  }

  group("Custom domains") {
    it("registers and resolves custom module errors") {
      cmeta_error_info_t info;

      (void)cmeta_error_unregister_domain(TEST_ERROR_DOMAIN);
      check_equal(cmeta_error_register_domain(&test_domain), SALTS_OK);

      info = cmeta_error_info(TEST_ERROR_AUTH);
      check_equal(info.code, TEST_ERROR_AUTH);
      check_equal(info.domain, SALTS_ERROR_DOMAIN_CUSTOM);
      check_equal(info.custom_domain, TEST_ERROR_DOMAIN);
      check_equal(info.domain_name, "test");
      check_equal(info.name, "TEST_EAUTH");
      check_equal(info.message, "test authentication failed");
      check_equal(cmeta_strerror(TEST_ERROR_PARSE), "test parse failed");

      check_equal(cmeta_error_register_domain(&test_domain), SALTS_EALREADY);
      check_equal(cmeta_error_unregister_domain(TEST_ERROR_DOMAIN), SALTS_OK);
      check_equal(cmeta_error_info(TEST_ERROR_AUTH).domain, SALTS_ERROR_DOMAIN_UNKNOWN);
    }

    it("rejects invalid custom domain descriptors") {
      static const cmeta_error_entry_t bad_errors[] = {
          {SALTS_ERROR_CUSTOM(TEST_ERROR_DOMAIN + 1, 1), "BAD", "bad domain"},
      };
      static const cmeta_error_domain_desc_t bad_domain = {
          TEST_ERROR_DOMAIN,
          "bad",
          bad_errors,
          sizeof(bad_errors) / sizeof(bad_errors[0]),
      };

      check_equal(cmeta_error_register_domain(NULL), SALTS_EINVAL);
      check_equal(cmeta_error_register_domain(&bad_domain), SALTS_EINVAL);
      check_equal(cmeta_error_unregister_domain(TEST_ERROR_DOMAIN), SALTS_ENOENT);
    }
  }
}
