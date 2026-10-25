/* Standalone explicit-manifest shared provider; no Salts runtime linked.
 * This only checks ELF symbol visibility and exact bootstrap manifest ABI.
 */
#include <salts/plugin_decl.h>

CMETA_PLUGIN_DECLARE_EMPTY(
    compiler_probe_dso,
    "salts.compiler.dso",
    (1u,2u,3u),
    CMETA_PLUGIN_PASSIVE());
