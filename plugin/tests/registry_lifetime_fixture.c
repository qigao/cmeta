#include <salts/plugin_decl.h>

/* A passive, no-export DSO exercises the real loader and Registry without
 * introducing a second lifecycle owner or source-level callback fixture. */
CMETA_PLUGIN_DECLARE_EMPTY(
    registry_lifetime_fixture, "test.plugin.registry.lifetime", (1,0,0),
    CMETA_PLUGIN_PASSIVE());
