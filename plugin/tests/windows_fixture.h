#ifndef SALTS_PLUGIN_WINDOWS_FIXTURE_H
#define SALTS_PLUGIN_WINDOWS_FIXTURE_H

#include <cmeta/plugin_decl.h>

#define PLUGIN_WINDOWS_EXPORT_ID "windows.transform"
#define PLUGIN_WINDOWS_CONTRACT_ID "test.plugin.windows.transform"
enum { PLUGIN_WINDOWS_CONTRACT_VERSION = 1 };

FunctionInvokeDecl(value, int, plugin_windows_transform,
    (int, input, CMETA_PARAM_IN));

#endif
