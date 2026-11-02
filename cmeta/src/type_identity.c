#include <cmeta/type_identity.h>

#include <string.h>

const cmeta_generic_desc cmeta_pair_generic_desc =
    CMETA_GENERIC_DESC_INIT("cmeta.Pair", "Pair", 2u, 2u,
                            CMETA_GENERIC_VALUE);
const cmeta_generic_desc cmeta_tuple_generic_desc =
    CMETA_GENERIC_DESC_INIT("cmeta.Tuple", "Tuple", 2u, 16u,
                            CMETA_GENERIC_VALUE);
const cmeta_generic_desc cmeta_option_generic_desc =
    CMETA_GENERIC_DESC_INIT("cmeta.Option", "Option", 1u, 1u,
                            CMETA_GENERIC_VALUE);
const cmeta_generic_desc cmeta_result_generic_desc =
    CMETA_GENERIC_DESC_INIT("cmeta.Result", "Result", 2u, 2u,
                            CMETA_GENERIC_VALUE);

static bool cmeta_nonempty(const char *s) {
    return s && s[0] != '\0';
}

static bool cmeta_generic_category_valid(cmeta_generic_category category) {
    switch (category) {
        case CMETA_GENERIC_VALUE:
        case CMETA_GENERIC_CONTAINER:
        case CMETA_GENERIC_HANDLE:
            return true;
    }
    return false;
}

bool cmeta_generic_desc_valid(const cmeta_generic_desc *desc) {
    return desc && cmeta_nonempty(desc->stable_id) &&
           cmeta_nonempty(desc->display_name) &&
           desc->min_arity <= desc->max_arity &&
           cmeta_generic_category_valid(desc->category);
}

bool cmeta_generic_accepts_arity(const cmeta_generic_desc *desc, size_t arity) {
    return cmeta_generic_desc_valid(desc) &&
           arity >= (size_t)desc->min_arity &&
           arity <= (size_t)desc->max_arity;
}

static bool cmeta_type_identity_valid_bounded(
    const cmeta_type_identity *identity, unsigned depth, size_t *remaining);

static bool cmeta_type_application_valid_bounded(
    const cmeta_generic_desc *constructor,
    const cmeta_type_identity *const *args,
    size_t arity, unsigned depth, size_t *remaining) {
    size_t i;
    if (!cmeta_generic_accepts_arity(constructor, arity))
        return false;
    if (arity != 0u && args == NULL)
        return false;
    for (i = 0u; i < arity; ++i) {
        if (!cmeta_type_identity_valid_bounded(args[i], depth + 1u, remaining))
            return false;
    }
    return true;
}

static bool cmeta_type_identity_valid_bounded(
    const cmeta_type_identity *identity, unsigned depth, size_t *remaining) {
    if (!identity || depth > CMETA_TYPE_IDENTITY_MAX_DEPTH || *remaining == 0u)
        return false;
    --*remaining;
    switch (identity->form) {
        case CMETA_TYPE_ATOM:
            return cmeta_nonempty(identity->stable_atom_id) &&
                   !identity->constructor && !identity->base &&
                   !identity->args && identity->arity == 0u;
        case CMETA_TYPE_POINTER:
        case CMETA_TYPE_CONST:
            return !identity->stable_atom_id && !identity->constructor &&
                   identity->base && !identity->args && identity->arity == 0u &&
                   cmeta_type_identity_valid_bounded(identity->base, depth + 1u, remaining);
        case CMETA_TYPE_APPLY:
            return !identity->stable_atom_id && !identity->base &&
                   cmeta_type_application_valid_bounded(identity->constructor,
                       identity->args, identity->arity, depth, remaining);
    }
    return false;
}

bool cmeta_type_identity_valid(const cmeta_type_identity *identity) {
    size_t remaining = CMETA_TYPE_IDENTITY_MAX_NODES;
    return cmeta_type_identity_valid_bounded(identity, 0u, &remaining);
}

bool cmeta_type_application_valid(
    const cmeta_generic_desc *constructor,
    const cmeta_type_identity *const *args, size_t arity) {
    const cmeta_type_identity application = {
        CMETA_TYPE_APPLY, NULL, constructor, NULL, args, arity
    };
    return cmeta_type_identity_valid(&application);
}

bool cmeta_generic_desc_equal(const cmeta_generic_desc *a,
                              const cmeta_generic_desc *b) {
    if (a == b) return a != NULL && cmeta_generic_desc_valid(a);
    return cmeta_generic_desc_valid(a) && cmeta_generic_desc_valid(b) &&
           strcmp(a->stable_id, b->stable_id) == 0;
}

/* Both graphs have passed the same finite admission before comparison.
 * A shared DAG is counted by visits, bounding expansion as well as depth. */
static bool cmeta_type_identity_equal_admitted(const cmeta_type_identity *a,
                                              const cmeta_type_identity *b) {
    size_t i;
    if (a == b) return a != NULL;
    if (!a || !b || a->form != b->form) return false;

    switch (a->form) {
        case CMETA_TYPE_ATOM:
            return cmeta_nonempty(a->stable_atom_id) &&
                   cmeta_nonempty(b->stable_atom_id) &&
                   strcmp(a->stable_atom_id, b->stable_atom_id) == 0;
        case CMETA_TYPE_POINTER:
        case CMETA_TYPE_CONST:
            return cmeta_type_identity_equal_admitted(a->base, b->base);
        case CMETA_TYPE_APPLY:
            if (!cmeta_generic_desc_equal(a->constructor, b->constructor) ||
                a->arity != b->arity)
                return false;
            if (a->arity != 0u && (!a->args || !b->args))
                return false;
            for (i = 0; i < a->arity; ++i)
                if (!cmeta_type_identity_equal_admitted(a->args[i], b->args[i]))
                    return false;
            return true;
    }
    return false;
}

bool cmeta_type_identity_equal(const cmeta_type_identity *a,
                               const cmeta_type_identity *b) {
    /* Pointer equality cannot authorize malformed or cyclic metadata. */
    return cmeta_type_identity_valid(a) && cmeta_type_identity_valid(b) &&
           cmeta_type_identity_equal_admitted(a, b);
}

bool cmeta_type_identity_is_application(const cmeta_type_identity *identity) {
    return identity && identity->form == CMETA_TYPE_APPLY;
}

const cmeta_generic_desc *
cmeta_type_identity_constructor(const cmeta_type_identity *identity) {
    return cmeta_type_identity_is_application(identity) ? identity->constructor : NULL;
}

size_t cmeta_type_identity_arity(const cmeta_type_identity *identity) {
    return cmeta_type_identity_is_application(identity) ? identity->arity : 0u;
}

const cmeta_type_identity *
cmeta_type_identity_argument(const cmeta_type_identity *identity, size_t index) {
    if (!cmeta_type_identity_is_application(identity) || !identity->args ||
        index >= identity->arity)
        return NULL;
    return identity->args[index];
}
