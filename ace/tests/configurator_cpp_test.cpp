#include <cmeta/configurator.h>
#include "tinytest.hpp"

suite("Salts Component C++ header") {
    it("exposes the bounded runtime without C++ wrappers") {
        check_equal(salts_configurator_status_string(SALTS_CONFIGURATOR_OK), "ok");
        check_equal(SALTS_CONFIGURATOR_INDEX_NONE, static_cast<size_t>(-1));
    }
}
