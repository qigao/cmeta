#include <cmeta/plugin_decl.h>

#ifndef REGISTRY_LIFETIME_FIXTURE_ID
#define REGISTRY_LIFETIME_FIXTURE_ID "test.plugin.registry.lifetime"
#endif

/* A passive, no-export DSO exercises the real loader and Registry without
 * introducing a second lifecycle owner or source-level callback fixture. */
CMETA_PLUGIN_DECLARE_EMPTY(
    registry_lifetime_fixture, REGISTRY_LIFETIME_FIXTURE_ID, (1,0,0),
    CMETA_PLUGIN_PASSIVE());

