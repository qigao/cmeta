#include <cmeta/object.h>

#define CMETA_OBJECT_FIELD_END(type_, member_) \
    (offsetof(type_, member_) + sizeof(((type_ *)0)->member_))
#define CMETA_OBJECT_FIELD_PROVIDER_ASSIGN_SIZE \
    CMETA_OBJECT_FIELD_END(cmeta_object_field_provider, assign)
#define CMETA_OBJECT_FIELD_PROVIDER_READ_SIZE \
    CMETA_OBJECT_FIELD_END(cmeta_object_field_provider, read)

static bool cmeta_object_field_provider_has_read(
    const cmeta_object_field_provider *provider) {
    return provider != NULL &&
           provider->size >= CMETA_OBJECT_FIELD_PROVIDER_READ_SIZE &&
           provider->read != NULL;
}

static void cmeta_object_clear(cmeta_object_ref *ref) {
    if (ref != NULL)
        *ref = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
}

bool cmeta_object_lifecycle_valid(
    const cmeta_object_lifecycle *lifecycle) {
    bool shared_pair;

    if (lifecycle == NULL || lifecycle->size < sizeof(*lifecycle))
        return false;
    shared_pair =
        (lifecycle->retain == NULL) == (lifecycle->release == NULL);
    return shared_pair &&
           (lifecycle->retain != NULL || lifecycle->destroy != NULL);
}

bool cmeta_object_operation_provider_valid(
    const cmeta_object_operation_provider *provider) {
    return provider != NULL && provider->size >= sizeof(*provider) &&
           cmeta_receiver_operation_set_valid(provider->operations) &&
           provider->bind != NULL;
}

bool cmeta_object_field_provider_valid(
    const cmeta_object_field_provider *provider) {
    bool has_read;

    if (provider == NULL ||
        provider->size < CMETA_OBJECT_FIELD_PROVIDER_ASSIGN_SIZE ||
        !cmeta_data_desc_valid(provider->data) ||
        provider->data->kind != CMETA_DATA_STRUCT ||
        provider->data->shape == NULL)
        return false;

    has_read = cmeta_object_field_provider_has_read(provider);
    return provider->assign != NULL || has_read;
}

static cmeta_status cmeta_object_contract_status(
    void *object, const cmeta_data_desc *data,
    const cmeta_object_field_provider *field_provider,
    const cmeta_receiver_operation_set *operations,
    const cmeta_object_operation_provider *operation_provider) {
    if (object == NULL || !cmeta_data_desc_valid(data))
        return CMETA_INVALID_ARGUMENT;
    if (data->storage_type == NULL ||
        !cmeta_type_desc_valid(data->storage_type))
        return CMETA_TRAIT_MISSING;
    if (operations != NULL) {
        if (!cmeta_receiver_operation_set_valid(operations))
            return CMETA_INVALID_ARGUMENT;
        if (!cmeta_type_equal(operations->receiver_type, data->storage_type))
            return CMETA_TYPE_MISMATCH;
    }
    if (field_provider != NULL) {
        if (!cmeta_object_field_provider_valid(field_provider) ||
            field_provider->data != data)
            return CMETA_INVALID_ARGUMENT;
    }
    if (operation_provider != NULL) {
        if (!cmeta_object_operation_provider_valid(operation_provider) ||
            operation_provider->operations != operations)
            return CMETA_INVALID_ARGUMENT;
    }
    return CMETA_OK;
}

bool cmeta_object_ref_valid(const cmeta_object_ref *ref) {
    if (ref == NULL || ref->size < sizeof(cmeta_object_ref) ||
        cmeta_object_contract_status(
            ref->object, ref->data, ref->field_provider, ref->operations,
            ref->operation_provider) != CMETA_OK)
        return false;

    switch (ref->lifetime) {
    case CMETA_OBJECT_LIFETIME_BORROWED:
        return ref->lifecycle == NULL;
    case CMETA_OBJECT_LIFETIME_SHARED:
        return cmeta_object_lifecycle_valid(ref->lifecycle) &&
               ref->lifecycle->retain != NULL &&
               ref->lifecycle->release != NULL;
    case CMETA_OBJECT_LIFETIME_OWNED:
        return cmeta_object_lifecycle_valid(ref->lifecycle) &&
               ref->lifecycle->destroy != NULL;
    default:
        return false;
    }
}

cmeta_status cmeta_object_borrow(
    cmeta_object_ref *out, void *object, const cmeta_data_desc *data,
    const cmeta_receiver_operation_set *operations) {
    cmeta_status status;

    if (out == NULL)
        return CMETA_INVALID_ARGUMENT;
    cmeta_object_clear(out);

    status = cmeta_object_contract_status(
        object, data, NULL, operations, NULL);
    if (status != CMETA_OK)
        return status;

    out->object = object;
    out->data = data;
    out->field_provider = NULL;
    out->operations = operations;
    out->operation_provider = NULL;
    out->lifetime = CMETA_OBJECT_LIFETIME_BORROWED;
    out->lifecycle = NULL;
    return CMETA_OK;
}

cmeta_status cmeta_object_borrow_with_provider(
    cmeta_object_ref *out, void *object, const cmeta_data_desc *data,
    const cmeta_object_operation_provider *provider) {
    cmeta_status status;

    if (out == NULL)
        return CMETA_INVALID_ARGUMENT;
    cmeta_object_clear(out);
    if (!cmeta_object_operation_provider_valid(provider))
        return CMETA_INVALID_ARGUMENT;

    status = cmeta_object_contract_status(
        object, data, NULL, provider->operations, provider);
    if (status != CMETA_OK)
        return status;

    out->object = object;
    out->data = data;
    out->field_provider = NULL;
    out->operations = provider->operations;
    out->operation_provider = provider;
    out->lifetime = CMETA_OBJECT_LIFETIME_BORROWED;
    out->lifecycle = NULL;
    return CMETA_OK;
}

cmeta_status cmeta_object_borrow_with_providers(
    cmeta_object_ref *out, void *object, const cmeta_data_desc *data,
    const cmeta_object_field_provider *field_provider,
    const cmeta_object_operation_provider *operation_provider) {
    const cmeta_receiver_operation_set *operations = NULL;
    cmeta_status status;

    if (out == NULL || (field_provider == NULL && operation_provider == NULL))
        return CMETA_INVALID_ARGUMENT;
    cmeta_object_clear(out);

    if (field_provider != NULL &&
        !cmeta_object_field_provider_valid(field_provider))
        return CMETA_INVALID_ARGUMENT;
    if (operation_provider != NULL) {
        if (!cmeta_object_operation_provider_valid(operation_provider))
            return CMETA_INVALID_ARGUMENT;
        operations = operation_provider->operations;
    }

    status = cmeta_object_contract_status(
        object, data, field_provider, operations, operation_provider);
    if (status != CMETA_OK)
        return status;

    out->object = object;
    out->data = data;
    out->field_provider = field_provider;
    out->operations = operations;
    out->operation_provider = operation_provider;
    out->lifetime = CMETA_OBJECT_LIFETIME_BORROWED;
    out->lifecycle = NULL;
    return CMETA_OK;
}

cmeta_status cmeta_object_share(
    cmeta_object_ref *ref, const cmeta_object_lifecycle *lifecycle) {
    cmeta_status status;

    if (!cmeta_object_ref_valid(ref) ||
        ref->lifetime != CMETA_OBJECT_LIFETIME_BORROWED ||
        !cmeta_object_lifecycle_valid(lifecycle) ||
        lifecycle->retain == NULL || lifecycle->release == NULL)
        return CMETA_INVALID_ARGUMENT;

    status = lifecycle->retain(lifecycle->context, ref->object);
    if (status != CMETA_OK)
        return status;

    ref->lifetime = CMETA_OBJECT_LIFETIME_SHARED;
    ref->lifecycle = lifecycle;
    return CMETA_OK;
}

cmeta_status cmeta_object_take(
    cmeta_object_ref *ref, const cmeta_object_lifecycle *lifecycle) {
    if (!cmeta_object_ref_valid(ref) ||
        ref->lifetime != CMETA_OBJECT_LIFETIME_BORROWED ||
        !cmeta_object_lifecycle_valid(lifecycle) ||
        lifecycle->destroy == NULL)
        return CMETA_INVALID_ARGUMENT;

    ref->lifetime = CMETA_OBJECT_LIFETIME_OWNED;
    ref->lifecycle = lifecycle;
    return CMETA_OK;
}

void cmeta_object_release(cmeta_object_ref *ref) {
    cmeta_object_lifetime lifetime;
    const cmeta_object_lifecycle *lifecycle;
    void *object;

    if (ref == NULL)
        return;

    lifetime = ref->lifetime;
    lifecycle = ref->lifecycle;
    object = ref->object;
    cmeta_object_clear(ref);

    if (lifecycle == NULL || object == NULL)
        return;
    if (lifetime == CMETA_OBJECT_LIFETIME_SHARED &&
        lifecycle->release != NULL)
        lifecycle->release(lifecycle->context, object);
    else if (lifetime == CMETA_OBJECT_LIFETIME_OWNED &&
             lifecycle->destroy != NULL)
        lifecycle->destroy(lifecycle->context, object);
}


static cmeta_status cmeta_object_field_resolve(
    const cmeta_object_ref *ref, const char *name,
    const cmeta_data_field_desc **out_field) {
    const cmeta_data_struct_shape *shape;
    const cmeta_data_field_desc *field;

    if (out_field != NULL)
        *out_field = NULL;

    if (!cmeta_object_ref_valid(ref) || name == NULL || name[0] == '\0')
        return CMETA_INVALID_ARGUMENT;
    if (ref->data->kind != CMETA_DATA_STRUCT || ref->data->shape == NULL)
        return CMETA_TRAIT_MISSING;

    shape = (const cmeta_data_struct_shape *)ref->data->shape;
    field = cmeta_data_struct_find_field(shape, name);
    if (field == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (!cmeta_data_desc_valid(field->value) ||
        field->value->storage_type == NULL)
        return CMETA_INVALID_ARGUMENT;

    if (out_field != NULL)
        *out_field = field;
    return CMETA_OK;
}

static cmeta_status cmeta_object_field_fixed_read(
    const cmeta_object_ref *ref, const cmeta_data_field_desc *field,
    const void **out_value) {
    const cmeta_data_desc *value;
    size_t owner_size;

    if (out_value == NULL || ref == NULL || field == NULL)
        return CMETA_INVALID_ARGUMENT;
    *out_value = NULL;

    if (field->offset == CMETA_FIELD_DYNAMIC_OFFSET)
        return CMETA_TRAIT_MISSING;

    value = field->value;
    owner_size = ref->data->storage_type->size;
    if (field->offset > owner_size ||
        value->storage_type->size > owner_size - field->offset ||
        (value->storage_type->align != 0u &&
         (field->offset % value->storage_type->align) != 0u))
        return CMETA_INVALID_ARGUMENT;

    *out_value = (const unsigned char *)ref->object + field->offset;
    return CMETA_OK;
}

cmeta_status cmeta_object_field_read(
    const cmeta_object_ref *ref, const char *name,
    const cmeta_data_desc **out_data, const void **out_value) {
    const cmeta_data_field_desc *field = NULL;
    const void *value = NULL;
    cmeta_status status;

    if (out_data == NULL || out_value == NULL)
        return CMETA_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_value = NULL;

    status = cmeta_object_field_resolve(ref, name, &field);
    if (status != CMETA_OK)
        return status;

    if (cmeta_object_field_provider_has_read(ref->field_provider)) {
        status = ref->field_provider->read(
            ref->field_provider->context, ref->object, field, &value);
        if (status != CMETA_OK)
            return status;
        if (value == NULL)
            return CMETA_CALLBACK_ERROR;
    } else {
        status = cmeta_object_field_fixed_read(ref, field, &value);
        if (status != CMETA_OK)
            return status;
    }

    *out_data = field->value;
    *out_value = value;
    return CMETA_OK;
}

cmeta_status cmeta_object_field_assign(
    cmeta_object_ref *ref, const char *name,
    const cmeta_data_desc *value_data, const void *value) {
    const cmeta_data_field_desc *field = NULL;
    cmeta_status status;

    if (value == NULL || !cmeta_data_desc_valid(value_data))
        return CMETA_INVALID_ARGUMENT;
    status = cmeta_object_field_resolve(ref, name, &field);
    if (status != CMETA_OK)
        return status;
    if (!cmeta_data_desc_equal(field->value, value_data))
        return CMETA_TYPE_MISMATCH;
    if (!cmeta_object_field_provider_valid(ref->field_provider) ||
        ref->field_provider->data != ref->data ||
        ref->field_provider->assign == NULL)
        return CMETA_TRAIT_MISSING;

    return ref->field_provider->assign(
        ref->field_provider->context, ref->object, field, value);
}

cmeta_receiver_resolve_status cmeta_object_operation_resolve(
    const cmeta_object_ref *ref, const cmeta_generic_desc *owner,
    const char *operation_name,
    const cmeta_type_desc *const *argument_types, size_t argument_count,
    cmeta_receiver_resolution *out) {
    if (!cmeta_object_ref_valid(ref) || out == NULL ||
        out->size < sizeof(*out))
        return CMETA_RECEIVER_RESOLVE_INVALID_ARGUMENT;
    if (ref->operations == NULL)
        return CMETA_RECEIVER_RESOLVE_INVALID_OPERATION_SET;
    return cmeta_receiver_operation_resolve(
        ref->operations, ref->data->storage_type, owner, operation_name,
        argument_types, argument_count, out);
}

cmeta_status cmeta_object_field_bind(const cmeta_object_ref *ref,
    const char *name, cmeta_object_field_binding *out) {
    const cmeta_data_field_desc *field = NULL;
    const void *fixed = NULL;
    cmeta_status status;
    if (out == NULL) return CMETA_INVALID_ARGUMENT;
    *out = (cmeta_object_field_binding)CMETA_OBJECT_FIELD_BINDING_INIT;
    status = cmeta_object_field_resolve(ref,name,&field);
    if (status != CMETA_OK) return status;
    if (!cmeta_object_field_provider_has_read(ref->field_provider)) {
        status = cmeta_object_field_fixed_read(ref,field,&fixed);
        if (status != CMETA_OK) return status;
    }
    out->object = ref->object;
    out->field = field;
    out->provider = ref->field_provider;
    out->fixed_value = fixed;
    return CMETA_OK;
}

cmeta_status cmeta_object_field_read_admitted(
    const cmeta_object_field_binding *binding, const void **out_value) {
    cmeta_status status;
    const void *value = NULL;
    if (out_value == NULL) return CMETA_INVALID_ARGUMENT;
    *out_value = NULL;
    if (binding == NULL || binding->object == NULL || binding->field == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (binding->fixed_value != NULL) value = binding->fixed_value;
    else {
        if (!cmeta_object_field_provider_has_read(binding->provider))
            return CMETA_TRAIT_MISSING;
        status = binding->provider->read(binding->provider->context,
            binding->object,binding->field,&value);
        if (status != CMETA_OK) return status;
        if (value == NULL) return CMETA_CALLBACK_ERROR;
    }
    *out_value = value;
    return CMETA_OK;
}

cmeta_status cmeta_object_field_assign_admitted(
    const cmeta_object_field_binding *binding, const void *value) {
    if (binding == NULL || binding->object == NULL || binding->field == NULL || value == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (binding->provider == NULL ||
        binding->provider->size < CMETA_OBJECT_FIELD_PROVIDER_ASSIGN_SIZE ||
        binding->provider->assign == NULL)
        return CMETA_TRAIT_MISSING;
    return binding->provider->assign(binding->provider->context,
        binding->object,binding->field,value);
}
