#include "windows_dependency.h"

int plugin_windows_dependency_transform(int value) {
    return value + PLUGIN_WINDOWS_BIAS;
}
