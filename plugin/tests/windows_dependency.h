#ifndef SALTS_PLUGIN_WINDOWS_DEPENDENCY_H
#define SALTS_PLUGIN_WINDOWS_DEPENDENCY_H

enum { PLUGIN_WINDOWS_BIAS = 7 };

#ifdef PLUGIN_WINDOWS_DEPENDENCY_BUILD
#define PLUGIN_WINDOWS_DEPENDENCY_API __declspec(dllexport)
#else
#define PLUGIN_WINDOWS_DEPENDENCY_API __declspec(dllimport)
#endif

PLUGIN_WINDOWS_DEPENDENCY_API int plugin_windows_dependency_transform(int value);

#endif
