#include "configurator_test_fixture.h"
#include <salts/component.h>
#include "tinytest.h"

/* Existing and candidate runtime types have deliberately distinct identity.
 * Both consume the same published provider ABI and CMeta descriptors. */
suite("Component to Configurator side-by-side parity") {
    it("runs two independent dependency graphs and destroys each exactly once") {
        unsigned old_clock = 0u;
        unsigned new_clock = 0u;
        int old_config = 5;
        int new_config = 23;
        test_provider_state old_logger, old_app, new_logger, new_app;
        salts_component_provider_binding old_providers[2], new_providers[2];
        salts_component_deployment old_deployments[2];
        salts_configurator_deployment new_deployments[2];
        salts_component_instance old_instances[2] = {0};
        salts_configurator_instance new_instances[2] = {0};
        salts_component_dependency old_dependencies[2] = {0};
        salts_component_dependency new_dependencies[2] = {0};
        size_t old_order[2] = {0}, new_order[2] = {0};
        salts_component_context old_context = SALTS_COMPONENT_CONTEXT_INIT;
        salts_configurator_context new_context = SALTS_CONFIGURATOR_CONTEXT_INIT;
        salts_component_service old_service = {0};
        salts_configurator_service new_service = {0};
        test_app old_iface = test_app_bind(NULL, NULL);
        test_app new_iface = test_app_bind(NULL, NULL);

        test_provider_state_init(&old_logger, 7, &old_clock);
        test_provider_state_init(&old_app, 0, &old_clock);
        test_provider_state_init(&new_logger, 11, &new_clock);
        test_provider_state_init(&new_app, 0, &new_clock);

        old_providers[0] = TEST_PROVIDER_BINDING(TestLogger, &old_logger, test_logger_create);
        old_providers[1] = TEST_PROVIDER_BINDING(TestApp, &old_app, test_app_create);
        new_providers[0] = TEST_PROVIDER_BINDING(TestLogger, &new_logger, test_logger_create);
        new_providers[1] = TEST_PROVIDER_BINDING(TestApp, &new_app, test_app_create);

        old_deployments[0] = (salts_component_deployment){&old_providers[0], NULL, NULL};
        old_deployments[1] = (salts_component_deployment){&old_providers[1], &cmeta_data_int, &old_config};
        new_deployments[0] = (salts_configurator_deployment){&new_providers[0], NULL, NULL};
        new_deployments[1] = (salts_configurator_deployment){&new_providers[1], &cmeta_data_int, &new_config};

        check_equal(salts_component_context_init(
            &old_context, old_deployments, 2u, NULL, 0u,
            old_instances, 2u, old_dependencies, 2u, old_order, 2u),
            SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_init(
            &new_context, new_deployments, 2u, NULL, 0u,
            new_instances, 2u, new_dependencies, 2u, new_order, 2u),
            SALTS_COMPONENT_OK);
        check_equal(salts_component_context_resolve(&old_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_resolve(&new_context), SALTS_COMPONENT_OK);
        check_equal(salts_component_context_start(&old_context), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_start(&new_context), SALTS_COMPONENT_OK);

        check_equal(old_clock, 2u);
        check_equal(new_clock, 2u);
        check_equal(salts_component_context_find_service(
            &old_context, test_app_interface(), &old_service), SALTS_COMPONENT_OK);
        check_equal(salts_configurator_context_find_service(
            &new_context, test_app_interface(), &new_service), SALTS_COMPONENT_OK);
        check_equal(test_app_borrow_from_object(
            old_service.object, old_service.interfaces, &old_iface), CMETA_OK);
        check_equal(test_app_borrow_from_object(
            new_service.object, new_service.interfaces, &new_iface), CMETA_OK);
        check_equal(test_app_value(&old_iface), 12);
        check_equal(test_app_value(&new_iface), 34);

        /* Shut down only the candidate; original live graph must be unaffected. */
        check_equal(salts_configurator_context_stop(&new_context), SALTS_COMPONENT_OK);
        check_equal(new_logger.destroys, 1u);
        check_equal(new_app.destroys, 1u);
        check_equal(new_logger.resource_releases, 1u);
        check_equal(new_app.resource_releases, 1u);
        check_equal(old_logger.destroys, 0u);
        check_equal(old_app.destroys, 0u);
        check_equal(test_app_value(&old_iface), 12);

        check_equal(salts_component_context_stop(&old_context), SALTS_COMPONENT_OK);
        check_equal(old_logger.destroys, 1u);
        check_equal(old_app.destroys, 1u);
        check_equal(old_logger.resource_releases, 1u);
        check_equal(old_app.resource_releases, 1u);
    }
}
