#ifndef CMETA_PLUGIN_NATIVE_FIXTURE_H
#define CMETA_PLUGIN_NATIVE_FIXTURE_H
#include <cmeta/plugin_decl.h>
#include <cmeta/native/thunk.h>
#include <cmeta/invokable.h>

/* Test-only borrowed publication. The host admits the exact target again;
 * provider-side admission never replaces the host's trust boundary. */
typedef struct plugin_native_offer {
    const cmeta_function_abi_desc *source;
    cmeta_native_binding binding;
    cmeta_invokable reference;
} plugin_native_offer;
static const cmeta_type_identity native_offer_id = CMETA_TYPE_ID_ATOM_INIT("test.plugin.native.offer.v1");
static const cmeta_type_identity native_offer_const = CMETA_TYPE_ID_CONST_INIT(&native_offer_id);
static const cmeta_type_identity native_offer_pointer = CMETA_TYPE_ID_POINTER_INIT(&native_offer_const);
static const cmeta_type_desc native_offer_type = {
    "const plugin_native_offer", sizeof(plugin_native_offer), CMETA_ALIGNOF(plugin_native_offer),
    CMETA_T_OBJECT, NULL, NULL, &native_offer_const
};
static const cmeta_type_desc native_offer_pointer_type = {
    "const plugin_native_offer *", sizeof(const plugin_native_offer *), CMETA_ALIGNOF(const plugin_native_offer *),
    CMETA_T_POINTER, &native_offer_type, NULL, &native_offer_pointer
};
#define NATIVE_OFFER_EXPORT "offer"
#define NATIVE_OFFER_CONTRACT "test.plugin.native.offer.v1"
enum { NATIVE_OFFER_VERSION = 1, NATIVE_PLUGIN_BIAS = 7, NATIVE_PLUGIN_BUDGET = 65536 };
Function0InvokeDeclAsAbiResult(value, const plugin_native_offer *, &native_offer_pointer_type,
    CMETA_ABI_OBJECT_POINTER, CMETA_RESULT_BORROWED, plugin_native_get_offer);
#endif
