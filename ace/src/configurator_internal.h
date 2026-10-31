#ifndef SALTS_CONFIGURATOR_INTERNAL_H
#define SALTS_CONFIGURATOR_INTERNAL_H

#include <salts/configurator.h>

/* Private ACE composition boundary. Configurator alone validates its graph,
 * instance and teardown records. These helpers run with exclusive access to
 * the context/storage and never invoke Provider callbacks. */
bool salts_configurator_context_active_valid_internal(
    const salts_configurator_context *context);
bool salts_configurator_context_settled_internal(
    const salts_configurator_context *context);

#endif
