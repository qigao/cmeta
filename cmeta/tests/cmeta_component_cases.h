#ifndef CMETA_COMPONENT_CASES_H
#define CMETA_COMPONENT_CASES_H

#include <cmeta/component.h>
#include <cmeta/fingerprint.h>
#include <cmeta/manifest_view.h>

#ifdef __cplusplus
#define COMPONENT_SIZE(value_) static_cast<size_t>(value_)
#else
#define COMPONENT_SIZE(value_) ((size_t)(value_))
#endif

#define COMPONENT_STORAGE_METHODS(X, I) \
    X(I, F0, int, ready, value, &cmeta_type_int, CMETA_ABI_SCALAR)

#define COMPONENT_LOGGER_METHODS(X, I) \
    X(I, FV0, void, flush, value, &cmeta_type_void, CMETA_ABI_VOID)

CMETA_INTERFACE(component_storage, COMPONENT_STORAGE_METHODS);
CMETA_INTERFACE(component_logger, COMPONENT_LOGGER_METHODS);

/* Opt-in dynamic-service checks are stricter than ordinary static metadata. */
#define COMPONENT_EXACT_METHODS(X, I) \
    X(I, FR1, int, get, value, &cmeta_type_int, CMETA_ABI_SCALAR, \
      CMETA_RESULT_VALUE, (int, input, CMETA_PARAM_IN, \
      &cmeta_type_int, CMETA_ABI_SCALAR))
#define COMPONENT_EXACT_POINTER_METHODS(X, I) \
    X(I, FR1, int, consume, value, &cmeta_type_int, CMETA_ABI_SCALAR, \
      CMETA_RESULT_VALUE, (void *, input, \
      CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &cmeta_type_void_ptr, \
      CMETA_ABI_OBJECT_POINTER))
#define COMPONENT_LEGACY_METHODS(X, I) X(I, R0, int, get, _)
#define COMPONENT_OWNING_METHODS(X, I) \
    X(I, FD0, void, destroy, stateful, &cmeta_type_void, CMETA_ABI_VOID)

CMETA_INTERFACE(component_exact, COMPONENT_EXACT_METHODS);
CMETA_INTERFACE(component_exact_pointer, COMPONENT_EXACT_POINTER_METHODS);
CMETA_INTERFACE(component_legacy, COMPONENT_LEGACY_METHODS);
CMETA_INTERFACE(component_owning, COMPONENT_OWNING_METHODS);

cmeta_component(PatternStorage,
    cmeta_provides(component_storage)
    cmeta_requires(component_logger));

cmeta_component_configured(PatternConfiguredStorage, &cmeta_data_int,
    cmeta_provides(component_storage)
    cmeta_requires(component_logger));

cmeta_component_empty(PatternEmpty);

cmeta_registry(pattern_components,
    cmeta_manifest_component_entry(
        "storage", cmeta_component_meta(PatternStorage))
    cmeta_manifest_component_entry(
        "configured-storage", cmeta_component_meta(PatternConfiguredStorage))
    cmeta_manifest_component_entry(
        "empty", cmeta_component_meta(PatternEmpty)));

suite("CMeta component declarations") {
    it("discovers one canonical component through an explicit manifest") {
        const cmeta_manifest_limits limits = {
            CMETA_MANIFEST_DEFAULT_ITEMS,
            CMETA_MANIFEST_DEFAULT_DEPTH,
            CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_component_desc *component = NULL;
        const cmeta_interface_desc *provided = NULL;
        const cmeta_interface_desc *required = NULL;

        check_equal(cmeta_manifest_get_component(
            &pattern_components, 0u, &limits, &component), CMETA_OK);
        check_not_null(component);
        check_equal(component->stable_id, "PatternStorage");
        check_equal(component->capability_count, COMPONENT_SIZE(2u));

        check_equal(cmeta_component_get_capability(
            component, 0u, CMETA_COMPONENT_PROVIDES, &limits, &provided), CMETA_OK);
        check_true(cmeta_interface_desc_equal(
            provided, component_storage_interface()));

        check_equal(cmeta_component_get_capability(
            component, 1u, CMETA_COMPONENT_REQUIRES, &limits, &required), CMETA_OK);
        check_true(cmeta_interface_desc_equal(
            required, component_logger_interface()));
    }

    it("keeps role admission explicit and fail closed") {
        const cmeta_manifest_limits limits = {
            CMETA_MANIFEST_DEFAULT_ITEMS,
            CMETA_MANIFEST_DEFAULT_DEPTH,
            CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_component_desc *component = NULL;
        const cmeta_interface_desc *unchanged = component_logger_interface();
        const cmeta_interface_desc *out = unchanged;

        check_equal(cmeta_manifest_get_component(
            &pattern_components, 0u, &limits, &component), CMETA_OK);
        check_equal(cmeta_component_get_capability(
            component, 0u, CMETA_COMPONENT_REQUIRES, &limits, &out),
            CMETA_TYPE_MISMATCH);
        check_true(out == unchanged);
    }

    it("separates stable component identity from capability-shape fingerprint") {
        const cmeta_fingerprint_limits limits = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH,
            CMETA_FINGERPRINT_DEFAULT_NODES,
            CMETA_FINGERPRINT_DEFAULT_ROWS,
            CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_component_desc *component = cmeta_component_meta(PatternStorage);
        cmeta_component_desc alias = *component;
        uint64_t original = 0u;
        uint64_t renamed = 0u;

        alias.stable_id = "test.other.storage";
        check_equal(cmeta_contract_fingerprint_component(
            component, &limits, &original), CMETA_OK);
        check_equal(cmeta_contract_fingerprint_component(
            &alias, &limits, &renamed), CMETA_OK);
        check_equal(original, renamed);
    }

    it("binds typed configuration through canonical DataDesc identity") {
        const cmeta_manifest_limits limits = {
            CMETA_MANIFEST_DEFAULT_ITEMS,
            CMETA_MANIFEST_DEFAULT_DEPTH,
            CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_component_desc *component = NULL;

        check_equal(cmeta_manifest_get_component(
            &pattern_components, 1u, &limits, &component), CMETA_OK);
        check_not_null(component);
        check_not_null(component->config);
        check_true(cmeta_data_desc_equal(component->config, &cmeta_data_int));
    }

    it("includes typed config contract in the component fingerprint") {
        const cmeta_fingerprint_limits limits = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH,
            CMETA_FINGERPRINT_DEFAULT_NODES,
            CMETA_FINGERPRINT_DEFAULT_ROWS,
            CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        uint64_t plain = 0u;
        uint64_t configured = 0u;

        check_equal(cmeta_contract_fingerprint_component(
            cmeta_component_meta(PatternStorage), &limits, &plain), CMETA_OK);
        check_equal(cmeta_contract_fingerprint_component(
            cmeta_component_meta(PatternConfiguredStorage), &limits, &configured), CMETA_OK);
        check_not_equal(plain, configured);
    }


    it("rejects kind-only container config through descriptor, manifest and fingerprint") {
        const cmeta_manifest_limits manifest_limits = {
            CMETA_MANIFEST_DEFAULT_ITEMS,
            CMETA_MANIFEST_DEFAULT_DEPTH,
            CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_fingerprint_limits fingerprint_limits = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH,
            CMETA_FINGERPRINT_DEFAULT_NODES,
            CMETA_FINGERPRINT_DEFAULT_ROWS,
            CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_data_kind kind_only_cases[] = {
            CMETA_DATA_SEQUENCE, CMETA_DATA_SET, CMETA_DATA_MAP
        };
        size_t i;

        for (i = 0u; i < sizeof(kind_only_cases) / sizeof(kind_only_cases[0]); ++i) {
            cmeta_data_desc config = cmeta_data_int;
            cmeta_component_desc component =
                *cmeta_component_meta(PatternConfiguredStorage);
            const cmeta_manifest_entry entries[] = {
                { "kind-only-config", CMETA_MANIFEST_COMPONENT, &component,
                  UINT64_C(0), UINT32_C(0) }
            };
            const cmeta_manifest manifest = {
                "kind-only-config", entries, COMPONENT_SIZE(1u),
                CMETA_MANIFEST_FORMAT_VERSION
            };
            const cmeta_component_desc *found =
                cmeta_component_meta(PatternConfiguredStorage);
            const cmeta_interface_desc *capability = component_logger_interface();
            uint64_t fingerprint = UINT64_C(0x1234);

            config.stable_id = "test.kind_only_config";
            config.display_name = "kind-only config";
            config.kind = kind_only_cases[i];
            config.storage_type = NULL;
            config.shape = NULL;
            config.collection_ops = NULL;
            config.map_ops = NULL;
            component.config = &config;

            /* This is a valid DataDesc for schema-only consumers, but not
             * for the exact native configuration consumed by Component. */
            check_true(cmeta_data_desc_valid(&config));
            check_false(cmeta_component_desc_valid(&component));
            check_equal(cmeta_manifest_get_component(
                &manifest, 0u, &manifest_limits, &found),
                CMETA_INVALID_ARGUMENT);
            check_true(found == cmeta_component_meta(PatternConfiguredStorage));
            check_equal(cmeta_component_get_capability(
                &component, 0u, CMETA_COMPONENT_PROVIDES,
                &manifest_limits, &capability), CMETA_INVALID_ARGUMENT);
            check_true(capability == component_logger_interface());
            check_equal(cmeta_contract_fingerprint_component(
                &component, &fingerprint_limits, &fingerprint),
                CMETA_INVALID_ARGUMENT);
            check_equal(fingerprint, UINT64_C(0x1234));
        }
    }

    it("rejects duplicate capability rows in validation and fingerprint") {
        const cmeta_fingerprint_limits limits = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH,
            CMETA_FINGERPRINT_DEFAULT_NODES,
            CMETA_FINGERPRINT_DEFAULT_ROWS,
            CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_component_capability capabilities[2] = {
            { CMETA_COMPONENT_PROVIDES, component_storage_interface() },
            { CMETA_COMPONENT_PROVIDES, component_storage_interface() }
        };
        cmeta_component_desc duplicate = {
            sizeof(cmeta_component_desc),
            CMETA_COMPONENT_DECLARATION_VERSION,
            "test.duplicate",
            NULL,
            capabilities,
            COMPONENT_SIZE(2u)
        };
        uint64_t fingerprint = UINT64_C(0);

        check_false(cmeta_component_desc_valid(&duplicate));
        check_equal(cmeta_contract_fingerprint_component(
            &duplicate, &limits, &fingerprint), CMETA_INVALID_ARGUMENT);
    }

    it("represents an explicit empty component without sentinel capabilities") {
        const cmeta_manifest_limits limits = {
            CMETA_MANIFEST_DEFAULT_ITEMS,
            CMETA_MANIFEST_DEFAULT_DEPTH,
            CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_component_desc *component = NULL;

        check_equal(cmeta_manifest_get_component(
            &pattern_components, 2u, &limits, &component), CMETA_OK);
        check_not_null(component);
        check_equal(component->capability_count, COMPONENT_SIZE(0u));
        check_null(component->capabilities);
    }
    it("admits an exact borrowed Interface contract under C11 and C++17") {
        check_true(cmeta_interface_borrowed_service_abi_valid(
            component_exact_interface()));
        check_true(cmeta_interface_borrowed_service_abi_valid(
            component_exact_pointer_interface()));
        check_true(cmeta_interface_borrowed_service_abi_valid(
            component_logger_interface()));
    }

    it("rejects legacy, unknown-result, and owning-self service methods") {
        check_true(cmeta_interface_desc_valid(component_legacy_interface()));
        check_false(cmeta_interface_borrowed_service_abi_valid(
            component_legacy_interface()));
        check_true(cmeta_interface_desc_valid(component_storage_interface()));
        check_false(cmeta_interface_borrowed_service_abi_valid(
            component_storage_interface()));
        check_true(cmeta_interface_desc_valid(component_owning_interface()));
        check_false(cmeta_interface_borrowed_service_abi_valid(
            component_owning_interface()));
    }

    it("rejects unspecified ABI and missing parameter direction") {
        const cmeta_interface_desc *canonical = component_exact_interface();
        cmeta_interface_method_desc method = canonical->methods[0];
        cmeta_function_desc function = *method.function;
        cmeta_param_desc param = function.params[0];
        cmeta_function_abi_desc abi = *method.abi;
        cmeta_interface_desc candidate = *canonical;
        const cmeta_abi_carrier unspecified[1] = {CMETA_ABI_UNSPECIFIED};

        method.function = &function;
        method.abi = &abi;
        function.params = &param;
        abi.function = &function;
        candidate.methods = &method;
        check_true(cmeta_interface_borrowed_service_abi_valid(&candidate));

        param.flags = CMETA_PARAM_UNKNOWN;
        check_true(cmeta_interface_desc_valid(&candidate));
        check_false(cmeta_interface_borrowed_service_abi_valid(&candidate));
        param.flags = CMETA_PARAM_IN;

        abi.param_carriers = unspecified;
        check_false(cmeta_interface_borrowed_service_abi_valid(&candidate));
        abi = *canonical->methods[0].abi;
        abi.function = &function;
        check_true(cmeta_interface_borrowed_service_abi_valid(&candidate));

        abi.return_carrier = CMETA_ABI_UNSPECIFIED;
        check_false(cmeta_interface_borrowed_service_abi_valid(&candidate));
    }

    it("rejects omitted pointer ownership and duplicate method names") {
        const cmeta_interface_desc *canonical = component_exact_pointer_interface();
        cmeta_interface_method_desc method = canonical->methods[0];
        cmeta_function_desc function = *method.function;
        cmeta_param_desc param = function.params[0];
        cmeta_function_abi_desc abi = *method.abi;
        cmeta_interface_desc candidate = *canonical;
        cmeta_interface_method_desc duplicate[2];

        method.function = &function;
        method.abi = &abi;
        function.params = &param;
        abi.function = &function;
        candidate.methods = &method;
        check_true(cmeta_interface_borrowed_service_abi_valid(&candidate));
        param.flags = CMETA_PARAM_IN;
        check_true(cmeta_interface_desc_valid(&candidate));
        check_false(cmeta_interface_borrowed_service_abi_valid(&candidate));

        duplicate[0] = canonical->methods[0];
        duplicate[1] = canonical->methods[0];
        candidate.methods = duplicate;
        candidate.method_count = COMPONENT_SIZE(2u);
        check_true(cmeta_interface_desc_valid(&candidate));
        check_false(cmeta_interface_borrowed_service_abi_valid(&candidate));
    }

}

#undef COMPONENT_SIZE

#endif /* CMETA_COMPONENT_CASES_H */
