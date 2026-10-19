#ifndef SALTS_CONFIGURATOR_TEST_CASES_H
#define SALTS_CONFIGURATOR_TEST_CASES_H

#include "configurator_test_fixture.h"

suite("Salts static Component Configurator") {
    it("resolves, activates and tears down dependencies deterministically") {
        unsigned clock = 0u;
        int app_config = 5;
        test_provider_state logger;
        test_provider_state app;
        salts_component_provider_binding providers[2];
        salts_configurator_deployment deployments[2];
        salts_configurator_instance instances[2];
        salts_component_dependency dependencies[2];
        size_t order[2];
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_configurator_service service;
        test_app app_interface = test_app_bind(NULL, NULL);

        test_provider_state_init(&logger, 7, &clock);
        test_provider_state_init(&app, 0, &clock);

        providers[0] = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create);
        deployments[0] = TEST_DEPLOYMENT(&providers[0], NULL, NULL);
        providers[1] = TEST_PROVIDER_BINDING(
            TestApp, &app, test_app_create);
        deployments[1] = TEST_DEPLOYMENT(
            &providers[1], &cmeta_data_int, &app_config);

        check_equal(salts_configurator_context_init(
            &context, deployments, 2u, NULL, 0u, instances, 2u,
            dependencies, 2u, order, 2u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(order[0], (size_t)0u);
        check_equal(order[1], (size_t)1u);

        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_ACTIVE);
        check_equal(logger.sequence, 1u);
        check_equal(app.sequence, 2u);
        check_equal(app.value, 12);

        check_equal(salts_configurator_context_find_service(
            &context, test_app_interface(), &service), SALTS_COMPONENT_OK);
        check_equal(test_app_borrow_from_object(
            service.object, service.interfaces, &app_interface), CMETA_OK);
        check_equal(test_app_value(&app_interface), 12);

        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_equal(logger.deactivates, 1u);
        check_equal(app.deactivates, 1u);
        check_equal(logger.destroys, 1u);
        check_equal(app.destroys, 1u);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_STOPPED);
        check_equal(context.activation_count, (size_t)0u);
    }

    it("refuses to reset a FAILED context with unsettled activation entries") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger, 7, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_true(instance.active);
        check_true(cmeta_object_ref_valid(&instance.object));
        check_equal(context.activation_count, (size_t)1u);

        /* Model an inconsistent FAILED marker while ObjectRef teardown
         * authority is still live. Init must reject without overwriting it. */
        context.state = SALTS_CONFIGURATOR_CONTEXT_FAILED;
        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_INVALID_STATE);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
        check_equal(context.activation_count, (size_t)1u);
        check_true(instance.active);
        check_true(cmeta_object_ref_valid(&instance.object));
        check_equal(logger.destroys, 0u);

        context.state = SALTS_CONFIGURATOR_CONTEXT_ACTIVE;
        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_STOPPED);
        check_equal(context.activation_count, (size_t)0u);
        check_equal(logger.destroys, 1u);
    }

    it("refuses reuse of a FAILED context with live ObjectRefs even when activation_count is zero") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        int value = 0;

        test_provider_state_init(&logger, 7, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_true(instance.active);
        check_true(cmeta_object_ref_valid(&instance.object));

        /* A zero plan count and FAILED enum do not discharge the live instance.
         * The reusable-state guard must inspect the owned ObjectRef prefix. */
        context.state = SALTS_CONFIGURATOR_CONTEXT_FAILED;
        context.failure.phase = SALTS_CONFIGURATOR_PHASE_ACTIVATE;
        context.activation_count = 0u;
        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_INVALID_STATE);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
        check_equal(context.activation_count, (size_t)0u);
        check_true(instance.active);
        check_true(cmeta_object_ref_valid(&instance.object));
        check_equal(logger.destroys, 0u);

        /* Restore the valid live owner state and settle it normally. */
        context.state = SALTS_CONFIGURATOR_CONTEXT_ACTIVE;
        context.activation_count = 1u;
        context.failure = (salts_configurator_failure){0};
        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_false(instance.active);
        check_false(cmeta_object_ref_valid(&instance.object));
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_STOPPED);
        check_equal(context.activation_count, (size_t)0u);
        check_equal(logger.destroys, 1u);
        (void)value;
    }

    it("rejects provider binding ABI mismatch before resolution") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger, 7, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create);
        provider.abi_version =
            SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION + UINT32_C(1);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_INVALID_COMPONENT);
        check_equal(logger.creates, 0u);
    }


    it("releases an owned partial ObjectRef on create failure") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger, 7, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create_then_fail);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context),
                    SALTS_COMPONENT_CREATE_FAILED);
        check_equal(logger.creates, 1u);
        check_equal(logger.activates, 0u);
        check_equal(logger.deactivates, 0u);
        check_equal(logger.destroys, 1u);
        check_equal(logger.resource_acquires, 0u);
        check_equal(logger.resource_releases, 0u);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
        check_equal(salts_configurator_context_failure(&context)->phase,
                    SALTS_CONFIGURATOR_PHASE_CREATE);
    }

    it("fails closed when a required provider is missing") {
        int config = 1;
        test_provider_state app;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        salts_component_dependency dependency;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&app, 0, NULL);
        provider = TEST_PROVIDER_BINDING(TestApp, &app, test_app_create);
        deployment = TEST_DEPLOYMENT(
            &provider, &cmeta_data_int, &config);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            &dependency, 1u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context),
                    SALTS_COMPONENT_MISSING_PROVIDER);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
    }

    it("rejects ambiguous providers instead of ranking implicitly") {
        int config = 1;
        test_provider_state logger_a;
        test_provider_state logger_b;
        test_provider_state app;
        salts_component_provider_binding providers[3];
        salts_configurator_deployment deployments[3];
        salts_configurator_instance instances[3];
        salts_component_dependency dependencies[2];
        size_t order[3];
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger_a, 1, NULL);
        test_provider_state_init(&logger_b, 2, NULL);
        test_provider_state_init(&app, 0, NULL);

        providers[0] = TEST_PROVIDER_BINDING(
            TestLogger, &logger_a, test_logger_create);
        deployments[0] = TEST_DEPLOYMENT(&providers[0], NULL, NULL);
        providers[1] = TEST_PROVIDER_BINDING(
            TestLoggerAlt, &logger_b, test_logger_create);
        deployments[1] = TEST_DEPLOYMENT(&providers[1], NULL, NULL);
        providers[2] = TEST_PROVIDER_BINDING(
            TestApp, &app, test_app_create);
        deployments[2] = TEST_DEPLOYMENT(
            &providers[2], &cmeta_data_int, &config);

        check_equal(salts_configurator_context_init(
            &context, deployments, 3u, NULL, 0u, instances, 3u,
            dependencies, 2u, order, 3u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context),
                    SALTS_COMPONENT_AMBIGUOUS_PROVIDER);
    }

    it("uses an explicit provider selection when multiple providers match") {
        int config = 10;
        test_provider_state logger_a;
        test_provider_state logger_b;
        test_provider_state app;
        salts_component_provider_binding providers[3];
        salts_configurator_deployment deployments[3];
        salts_configurator_selection selection;
        salts_configurator_instance instances[3];
        salts_component_dependency dependencies[2];
        size_t order[3];
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_configurator_service service;
        test_app app_interface = test_app_bind(NULL, NULL);

        test_provider_state_init(&logger_a, 1, NULL);
        test_provider_state_init(&logger_b, 8, NULL);
        test_provider_state_init(&app, 0, NULL);

        providers[0] = TEST_PROVIDER_BINDING(
            TestLogger, &logger_a, test_logger_create);
        deployments[0] = TEST_DEPLOYMENT(&providers[0], NULL, NULL);
        providers[1] = TEST_PROVIDER_BINDING(
            TestLoggerAlt, &logger_b, test_logger_create);
        deployments[1] = TEST_DEPLOYMENT(&providers[1], NULL, NULL);
        providers[2] = TEST_PROVIDER_BINDING(
            TestApp, &app, test_app_create);
        deployments[2] = TEST_DEPLOYMENT(
            &providers[2], &cmeta_data_int, &config);

        selection = (salts_configurator_selection){
            "TestApp", test_log_interface(), "TestLoggerAlt"
        };

        check_equal(salts_configurator_context_init(
            &context, deployments, 3u, &selection, 1u,
            instances, 3u, dependencies, 2u, order, 3u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);

        check_equal(salts_configurator_context_find_service(
            &context, test_app_interface(), &service), SALTS_COMPONENT_OK);
        check_equal(test_app_borrow_from_object(
            service.object, service.interfaces, &app_interface), CMETA_OK);
        check_equal(test_app_value(&app_interface), 18);

        check_equal(salts_configurator_context_find_service_from(
            &context, "TestLoggerAlt", test_log_interface(), &service),
            SALTS_COMPONENT_OK);
        {
            test_log selected = test_log_bind(NULL, NULL);
            check_equal(test_log_borrow_from_object(
                service.object, service.interfaces, &selected), CMETA_OK);
            check_equal(test_log_get(&selected), 8);
        }

        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
    }

    it("detects dependency cycles before any create callback") {
        test_provider_state a;
        test_provider_state b;
        salts_component_provider_binding providers[2];
        salts_configurator_deployment deployments[2];
        salts_configurator_instance instances[2];
        salts_component_dependency dependencies[2];
        size_t order[2];
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&a, 1, NULL);
        test_provider_state_init(&b, 2, NULL);

        providers[0] = TEST_PROVIDER_BINDING(
            TestCycleA, &a, test_cycle_create);
        deployments[0] = TEST_DEPLOYMENT(&providers[0], NULL, NULL);
        providers[1] = TEST_PROVIDER_BINDING(
            TestCycleB, &b, test_cycle_create);
        deployments[1] = TEST_DEPLOYMENT(&providers[1], NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, deployments, 2u, NULL, 0u, instances, 2u,
            dependencies, 2u, order, 2u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context),
                    SALTS_COMPONENT_DEPENDENCY_CYCLE);
        check_equal(a.creates, 0u);
        check_equal(b.creates, 0u);
    }

    it("rejects a configuration type mismatch before resolution") {
        bool wrong_config = true;
        test_provider_state app;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        salts_component_dependency dependency;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&app, 0, NULL);
        provider = TEST_PROVIDER_BINDING(TestApp, &app, test_app_create);
        deployment = TEST_DEPLOYMENT(
            &provider, &cmeta_data_bool, &wrong_config);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            &dependency, 1u, &order, 1u), SALTS_COMPONENT_CONFIG_MISMATCH);
        check_equal(app.creates, 0u);
    }

    it("rejects a provider that cannot project its declared service") {
        test_provider_state broken;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&broken, 9, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestBrokenProvider, &broken, test_cycle_create);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context),
                    SALTS_COMPONENT_INTERFACE_UNAVAILABLE);
        check_equal(broken.activates, 1u);
        check_equal(broken.deactivates, 1u);
        check_equal(broken.destroys, 1u);
        check_equal(broken.resource_acquires, 1u);
        check_equal(broken.resource_releases, 1u);
        check_false(broken.resource_live);
        check_equal(salts_configurator_context_failure(&context)->phase,
                    SALTS_CONFIGURATOR_PHASE_PROVIDE);
    }

    it("keeps two static component contexts fully independent") {
        test_provider_state left;
        test_provider_state right;
        salts_component_provider_binding left_provider;
        salts_component_provider_binding right_provider;
        salts_configurator_deployment left_deployment;
        salts_configurator_deployment right_deployment;
        salts_configurator_instance left_instance;
        salts_configurator_instance right_instance;
        size_t left_order;
        size_t right_order;
        salts_configurator_context left_context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_configurator_context right_context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_configurator_service left_service;
        salts_configurator_service right_service;
        test_log left_log = test_log_bind(NULL, NULL);
        test_log right_log = test_log_bind(NULL, NULL);

        test_provider_state_init(&left, 11, NULL);
        test_provider_state_init(&right, 22, NULL);

        left_provider = TEST_PROVIDER_BINDING(
            TestLogger, &left, test_logger_create);
        left_deployment = TEST_DEPLOYMENT(&left_provider, NULL, NULL);
        right_provider = TEST_PROVIDER_BINDING(
            TestLogger, &right, test_logger_create);
        right_deployment = TEST_DEPLOYMENT(&right_provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &left_context, &left_deployment, 1u, NULL, 0u, &left_instance, 1u,
            NULL, 0u, &left_order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_init(
            &right_context, &right_deployment, 1u, NULL, 0u, &right_instance, 1u,
            NULL, 0u, &right_order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&left_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&right_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&left_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&right_context), SALTS_COMPONENT_OK);

        check_equal(salts_configurator_context_find_service(
            &left_context, test_log_interface(), &left_service), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_find_service(
            &right_context, test_log_interface(), &right_service), SALTS_COMPONENT_OK);
        check_equal(test_log_borrow_from_object(
            left_service.object, left_service.interfaces, &left_log), CMETA_OK);
        check_equal(test_log_borrow_from_object(
            right_service.object, right_service.interfaces, &right_log), CMETA_OK);
        check_equal(test_log_get(&left_log), 11);
        check_equal(test_log_get(&right_log), 22);

        check_equal(salts_configurator_context_stop(&left_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_stop(&right_context), SALTS_COMPONENT_OK);
    }


    it("rejects reinit in READY, RESOLVED and ACTIVE without losing owners") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_configurator_service service;

        test_provider_state_init(&logger, 13, NULL);
        provider = TEST_PROVIDER_BINDING(TestLogger, &logger, test_logger_create);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_init(
            &context, NULL, 0u, NULL, 0u, NULL, 0u,
            NULL, 0u, NULL, 0u), SALTS_COMPONENT_INVALID_STATE);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_READY);
        check_true(context.deployments == &deployment);

        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_init(
            &context, NULL, 0u, NULL, 0u, NULL, 0u,
            NULL, 0u, NULL, 0u), SALTS_COMPONENT_INVALID_STATE);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_RESOLVED);
        check_equal(context.activation_count, (size_t)1u);

        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_true(cmeta_object_ref_valid(&instance.object));
        check_true(instance.active);
        check_equal(logger.resource_acquires, 1u);

        check_equal(salts_configurator_context_init(
            &context, NULL, 0u, NULL, 0u, NULL, 0u,
            NULL, 0u, NULL, 0u), SALTS_COMPONENT_INVALID_STATE);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_ACTIVE);
        check_true(context.instances == &instance);
        check_true(cmeta_object_ref_valid(&instance.object));
        check_equal(logger.destroys, 0u);

        check_equal(salts_configurator_context_find_service(
            &context, test_log_interface(), &service), SALTS_COMPONENT_OK);
        check_true(service.object == &instance.object);

        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_equal(logger.deactivates, 1u);
        check_equal(logger.destroys, 1u);
        check_equal(logger.resource_acquires, 1u);
        check_equal(logger.resource_releases, 1u);
        check_false(logger.resource_live);

        /* STOPPED reuse is permitted only after owned resources are closed. */
        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_equal(logger.creates, 2u);
        check_equal(logger.activates, 2u);
        check_equal(logger.deactivates, 2u);
        check_equal(logger.destroys, 2u);
        check_equal(logger.resource_acquires, 2u);
        check_equal(logger.resource_releases, 2u);
    }

    it("reuses a fully rolled-back FAILED context without double cleanup") {
        test_provider_state logger;
        salts_component_provider_binding provider;
        salts_configurator_deployment deployment;
        salts_configurator_instance instance;
        size_t order;
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger, 3, NULL);
        provider = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create_then_fail);
        deployment = TEST_DEPLOYMENT(&provider, NULL, NULL);

        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context),
                    SALTS_COMPONENT_CREATE_FAILED);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
        check_equal(logger.destroys, 1u);
        check_false(cmeta_object_ref_valid(&instance.object));

        provider.create = test_logger_create;
        check_equal(salts_configurator_context_init(
            &context, &deployment, 1u, NULL, 0u, &instance, 1u,
            NULL, 0u, &order, 1u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_stop(&context), SALTS_COMPONENT_OK);
        check_equal(logger.creates, 2u);
        check_equal(logger.activates, 1u);
        check_equal(logger.deactivates, 1u);
        check_equal(logger.destroys, 2u);
        check_equal(logger.resource_acquires, 1u);
        check_equal(logger.resource_releases, 1u);
    }

    it("rolls back prior active components exactly once on activation failure") {
        int config = 3;
        test_provider_state logger;
        test_provider_state app;
        salts_component_provider_binding providers[2];
        salts_configurator_deployment deployments[2];
        salts_configurator_instance instances[2];
        salts_component_dependency dependencies[2];
        size_t order[2];
        salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;

        test_provider_state_init(&logger, 4, NULL);
        test_provider_state_init(&app, 0, NULL);
        app.fail_activate = true;

        providers[0] = TEST_PROVIDER_BINDING(
            TestLogger, &logger, test_logger_create);
        deployments[0] = TEST_DEPLOYMENT(&providers[0], NULL, NULL);
        providers[1] = TEST_PROVIDER_BINDING(
            TestApp, &app, test_app_create);
        deployments[1] = TEST_DEPLOYMENT(
            &providers[1], &cmeta_data_int, &config);

        check_equal(salts_configurator_context_init(
            &context, deployments, 2u, NULL, 0u, instances, 2u,
            dependencies, 2u, order, 2u), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&context),
                    SALTS_COMPONENT_ACTIVATE_FAILED);

        check_equal(logger.deactivates, 1u);
        check_equal(logger.destroys, 1u);
        check_equal(app.deactivates, 0u);
        check_equal(app.destroys, 1u);
        check_equal(logger.resource_acquires, 1u);
        check_equal(logger.resource_releases, 1u);
        check_false(logger.resource_live);
        check_equal(app.resource_acquires, 1u);
        check_equal(app.resource_releases, 1u);
        check_false(app.resource_live);
        check_equal(context.state, SALTS_CONFIGURATOR_CONTEXT_FAILED);
        check_equal(salts_configurator_context_failure(&context)->phase,
                    SALTS_CONFIGURATOR_PHASE_ACTIVATE);
    }
}

#endif
