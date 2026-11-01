#include <cmeta/plugin.h>

static bool bounded_string_valid(const char *value, size_t max_bytes) {
    size_t index;
    if (value == NULL || max_bytes == 0u) return false;
    for (index = 0u; index <= max_bytes; ++index) {
        if (value[index] == '\0') return index != 0u;
    }
    return false;
}

static bool bounded_string_equal(const char *left, const char *right,
                                 size_t max_bytes) {
    size_t index;
    if (left == NULL || right == NULL || max_bytes == 0u) return false;
    for (index = 0u; index <= max_bytes; ++index) {
        if (left[index] != right[index]) return false;
        if (left[index] == '\0') return index != 0u;
    }
    return false;
}

static bool function_abi_complete(const cmeta_function_abi_desc *abi) {
    size_t index;

    if (!cmeta_function_abi_desc_valid(abi) ||
        abi->return_carrier == CMETA_ABI_UNSPECIFIED)
        return false;

    for (index = 0u; index < abi->param_count; ++index) {
        if (abi->param_carriers[index] == CMETA_ABI_UNSPECIFIED)
            return false;
    }

    return true;
}

static cmeta_plugin_status validate_export(const cmeta_plugin_export *entry) {
    if (entry == NULL || entry->struct_size != CMETA_PLUGIN_EXPORT_SIZE)
        return CMETA_PLUGIN_INVALID_MANIFEST;
    if (!bounded_string_valid(entry->export_id, CMETA_PLUGIN_EXPORT_ID_MAX) ||
        !bounded_string_valid(entry->contract_id,
                              CMETA_PLUGIN_CONTRACT_ID_MAX) ||
        entry->contract_version == 0u)
        return CMETA_PLUGIN_INVALID_MANIFEST;

    switch (entry->kind) {
    case CMETA_PLUGIN_EXPORT_INTERFACE:
        if (entry->value.interface.value == NULL ||
            !cmeta_interface_desc_valid(entry->value.interface.desc))
            return CMETA_PLUGIN_INVALID_MANIFEST;
        break;

    case CMETA_PLUGIN_EXPORT_FUNCTION:
        if (!cmeta_function_desc_valid(entry->value.function.desc) ||
            !function_abi_complete(entry->value.function.abi) ||
            entry->value.function.abi->function != entry->value.function.desc ||
            entry->value.function.invoke == NULL)
            return CMETA_PLUGIN_INVALID_MANIFEST;
        break;

    default:
        return CMETA_PLUGIN_INVALID_MANIFEST;
    }

    return CMETA_PLUGIN_OK;
}

bool cmeta_plugin_export_has_capabilities(const cmeta_plugin_export *entry,
                                          uint64_t required) {
    return entry != NULL && (entry->capabilities & required) == required;
}

static bool contract_key_equal(const cmeta_plugin_export *entry,
                               const char *contract_id,
                               uint32_t contract_version) {
    return entry != NULL && contract_version != 0u &&
           bounded_string_valid(contract_id, CMETA_PLUGIN_CONTRACT_ID_MAX) &&
           entry->contract_version == contract_version &&
           bounded_string_equal(entry->contract_id, contract_id,
                                CMETA_PLUGIN_CONTRACT_ID_MAX);
}

cmeta_plugin_status cmeta_plugin_export_require_interface(
    const cmeta_plugin_export *entry,
    const char *contract_id,
    uint32_t contract_version,
    uint64_t required_capabilities,
    const cmeta_interface_desc *expected_interface) {
    cmeta_plugin_status status = validate_export(entry);

    if (status != CMETA_PLUGIN_OK) return status;
    if (contract_version == 0u ||
        !bounded_string_valid(contract_id, CMETA_PLUGIN_CONTRACT_ID_MAX) ||
        !cmeta_interface_desc_valid(expected_interface))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    if (entry->kind != CMETA_PLUGIN_EXPORT_INTERFACE ||
        !contract_key_equal(entry, contract_id, contract_version) ||
        !cmeta_plugin_export_has_capabilities(entry, required_capabilities) ||
        !cmeta_interface_desc_equal(entry->value.interface.desc,
                                           expected_interface))
        return CMETA_PLUGIN_INCOMPATIBLE_CONTRACT;

    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_export_require_function(
    const cmeta_plugin_export *entry,
    const char *contract_id,
    uint32_t contract_version,
    uint64_t required_capabilities) {
    cmeta_plugin_status status = validate_export(entry);

    if (status != CMETA_PLUGIN_OK) return status;
    if (contract_version == 0u ||
        !bounded_string_valid(contract_id, CMETA_PLUGIN_CONTRACT_ID_MAX))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    if (entry->kind != CMETA_PLUGIN_EXPORT_FUNCTION ||
        !contract_key_equal(entry, contract_id, contract_version) ||
        !cmeta_plugin_export_has_capabilities(entry, required_capabilities))
        return CMETA_PLUGIN_INCOMPATIBLE_CONTRACT;

    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_manifest_validate(
    const cmeta_plugin_manifest *manifest) {
    size_t index;
    size_t other;
    bool has_lifecycle;

    if (manifest == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    if (manifest->struct_size != CMETA_PLUGIN_MANIFEST_SIZE)
        return CMETA_PLUGIN_INVALID_MANIFEST;
    if (manifest->abi_version != CMETA_PLUGIN_ABI_VERSION)
        return CMETA_PLUGIN_UNSUPPORTED_ABI;
    if (!bounded_string_valid(manifest->plugin_id, CMETA_PLUGIN_ID_MAX))
        return CMETA_PLUGIN_INVALID_MANIFEST;
    if (manifest->export_count > CMETA_PLUGIN_MAX_EXPORTS)
        return CMETA_PLUGIN_CAPACITY_EXCEEDED;
    if (manifest->export_count != 0u && manifest->exports == NULL)
        return CMETA_PLUGIN_INVALID_MANIFEST;

    has_lifecycle = manifest->start != NULL ||
                    manifest->request_stop != NULL ||
                    manifest->is_quiescent != NULL ||
                    manifest->destroy != NULL;
    if (has_lifecycle) {
        if (manifest->self == NULL ||
            manifest->start == NULL ||
            manifest->request_stop == NULL ||
            manifest->is_quiescent == NULL ||
            manifest->destroy == NULL)
            return CMETA_PLUGIN_INVALID_MANIFEST;
    } else if (manifest->self != NULL) {
        return CMETA_PLUGIN_INVALID_MANIFEST;
    }

    for (index = 0u; index < manifest->export_count; ++index) {
        cmeta_plugin_status status = validate_export(&manifest->exports[index]);
        if (status != CMETA_PLUGIN_OK) return status;

        for (other = 0u; other < index; ++other) {
            if (bounded_string_equal(manifest->exports[index].export_id,
                                     manifest->exports[other].export_id,
                                     CMETA_PLUGIN_EXPORT_ID_MAX))
                return CMETA_PLUGIN_DUPLICATE_EXPORT;
        }
    }

    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_manifest_find_export(
    const cmeta_plugin_manifest *manifest,
    const char *export_id,
    const cmeta_plugin_export **out_export) {
    size_t index;
    cmeta_plugin_status status;

    if (out_export == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_export = NULL;

    if (!bounded_string_valid(export_id, CMETA_PLUGIN_EXPORT_ID_MAX))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    status = cmeta_plugin_manifest_validate(manifest);
    if (status != CMETA_PLUGIN_OK) return status;

    for (index = 0u; index < manifest->export_count; ++index) {
        if (bounded_string_equal(manifest->exports[index].export_id,
                                 export_id, CMETA_PLUGIN_EXPORT_ID_MAX)) {
            *out_export = &manifest->exports[index];
            return CMETA_PLUGIN_OK;
        }
    }

    return CMETA_PLUGIN_UNKNOWN_EXPORT;
}

const char *cmeta_plugin_status_string(cmeta_plugin_status status) {
    switch (status) {
    case CMETA_PLUGIN_OK: return "ok";
    case CMETA_PLUGIN_INVALID_ARGUMENT: return "invalid_argument";
    case CMETA_PLUGIN_INVALID_MANIFEST: return "invalid_manifest";
    case CMETA_PLUGIN_UNSUPPORTED_ABI: return "unsupported_abi";
    case CMETA_PLUGIN_DUPLICATE_PLUGIN_ID: return "duplicate_plugin_id";
    case CMETA_PLUGIN_DUPLICATE_EXPORT: return "duplicate_export";
    case CMETA_PLUGIN_UNKNOWN_EXPORT: return "unknown_export";
    case CMETA_PLUGIN_INCOMPATIBLE_CONTRACT: return "incompatible_contract";
    case CMETA_PLUGIN_CAPACITY_EXCEEDED: return "capacity_exceeded";
    case CMETA_PLUGIN_ALLOCATION_FAILED: return "allocation_failed";
    case CMETA_PLUGIN_LOAD_FAILED: return "load_failed";
    case CMETA_PLUGIN_QUERY_MISSING: return "query_missing";
    case CMETA_PLUGIN_QUERY_REJECTED: return "query_rejected";
    case CMETA_PLUGIN_UNKNOWN_PLUGIN: return "unknown_plugin";
    case CMETA_PLUGIN_STALE: return "stale";
    case CMETA_PLUGIN_UNLOAD_FAILED: return "unload_failed";
    case CMETA_PLUGIN_ALREADY: return "already";
    case CMETA_PLUGIN_BUSY: return "busy";
    case CMETA_PLUGIN_INVALID_STATE: return "invalid_state";
    default: return "unknown";
    }
}
