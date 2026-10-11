#include "plugin_loader_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(FARPROC) == sizeof(cmeta_plugin_query_fn),
               "GetProcAddress representation must fit plugin query pointer");

cmeta_plugin_status cmeta_plugin_platform_close(
    cmeta_plugin_library *library) {
    HMODULE module;

    if (library == NULL || library->handle == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    module = (HMODULE)library->handle;
    if (!FreeLibrary(module))
        return CMETA_PLUGIN_UNLOAD_FAILED;

    library->handle = NULL;
    return CMETA_PLUGIN_OK;
}

static bool cmeta_plugin_windows_explicit_path(const wchar_t *path) {
    const wchar_t *cursor;
    if (path == NULL)
        return false;
    for (cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/')
            return true;
    }
    return false;
}

static wchar_t *cmeta_plugin_windows_absolute_path(const wchar_t *path) {
    DWORD required;
    DWORD written;
    wchar_t *absolute;

    if (path == NULL)
        return NULL;

    required = GetFullPathNameW(path, 0u, NULL, NULL);
    if (required == 0u)
        return NULL;

    absolute = (wchar_t *)malloc((size_t)required * sizeof(*absolute));
    if (absolute == NULL)
        return NULL;

    written = GetFullPathNameW(path, required, absolute, NULL);
    if (written == 0u || written >= required) {
        free(absolute);
        return NULL;
    }
    return absolute;
}

cmeta_plugin_status cmeta_plugin_platform_open(
    const char *path,
    cmeta_plugin_library *out_library,
    cmeta_plugin_query_fn *out_query) {
    int wide_length;
    wchar_t *wide_path;
    wchar_t *absolute_path = NULL;
    HMODULE module;
    FARPROC symbol;

    if (path == NULL || out_library == NULL || out_query == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    out_library->handle = NULL;
    *out_query = NULL;

    wide_length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_length <= 0)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    wide_path = (wchar_t *)malloc((size_t)wide_length * sizeof(*wide_path));
    if (wide_path == NULL)
        return CMETA_PLUGIN_ALLOCATION_FAILED;

    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
            wide_path, wide_length) != wide_length) {
        free(wide_path);
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    }

    if (cmeta_plugin_windows_explicit_path(wide_path)) {
        absolute_path = cmeta_plugin_windows_absolute_path(wide_path);
        if (absolute_path == NULL) {
            free(wide_path);
            return CMETA_PLUGIN_LOAD_FAILED;
        }
        /* Keep dependency discovery within the plugin directory and the
         * application's explicitly trusted Windows search directories. */
        module = LoadLibraryExW(
            absolute_path, NULL,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        free(absolute_path);
    } else {
        /* Bare-name lookup remains the caller's existing platform policy. */
        module = LoadLibraryW(wide_path);
    }
    free(wide_path);
    if (module == NULL)
        return CMETA_PLUGIN_LOAD_FAILED;

    symbol = GetProcAddress(module, CMETA_PLUGIN_QUERY_SYMBOL);
    if (symbol == NULL) {
        cmeta_plugin_library cleanup = {(void *)module};
        cmeta_plugin_status cleanup_status =
            cmeta_plugin_platform_close(&cleanup);
        return cleanup_status == CMETA_PLUGIN_OK
            ? CMETA_PLUGIN_QUERY_MISSING
            : cleanup_status;
    }

    memcpy(out_query, &symbol, sizeof(*out_query));
    out_library->handle = (void *)module;
    return CMETA_PLUGIN_OK;
}
