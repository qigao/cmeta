#include "windows_fixture.h"
#include "windows_dependency.h"

int plugin_windows_transform(int input) {
    return plugin_windows_dependency_transform(input);
}

#define WINDOWS_EXPORTS(X) \
    X(function, plugin_windows_transform, PLUGIN_WINDOWS_EXPORT_ID, \
      PLUGIN_WINDOWS_CONTRACT_ID, PLUGIN_WINDOWS_CONTRACT_VERSION, 0)

CMETA_PLUGIN_DECLARE(windows_fixture, "test.plugin.windows.search", (1,0,0),
    WINDOWS_EXPORTS, CMETA_PLUGIN_PASSIVE());
