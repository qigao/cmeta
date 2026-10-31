#ifndef SALTS_NATIVE_IO_ACE_TOKEN_H
#define SALTS_NATIVE_IO_ACE_TOKEN_H

#include <salts/native_io.h>
#include <salts/error_codes.h>

/*
 * POSA2 Asynchronous Completion Token (ACT).
 *
 * The NativeIO request, endpoint and terminal completion are authoritative.
 * This header only produces a native-typed, caller-owned association for one
 * already admitted operation; no table, scheduler, second terminal or retain.
 *
 * Bind AFTER successful submit/prepare. Keep context and borrowed request
 * payload valid until the terminal completion is observed and settled; cancel
 * is a request, not a terminal. Failed bind does not release any resources.
 * One exact matching terminal consumes the token; a stale/replayed/wrong
 * generation/user_data completion cannot consume it.
 *
 * A token is single-owner storage. It is neither atomic nor safely copyable
 * while live; callers serialize settle with the NativeIO progress owner.
 * Context ownership and callback/Plugin lease remain with the caller.
 */
#define NATIVE_IO_ACE_TOKEN_TYPE(name_, context_type_) \
    typedef struct name_ { \
        native_io_request request; \
        native_io_endpoint endpoint; \
        uintptr_t correlation; \
        context_type_ *context; \
        const struct name_ *owner_address; \
        bool active; \
    } name_; \
    static inline int name_##_bind( \
        name_ *token, native_io_request request, native_io_endpoint endpoint, \
        uintptr_t correlation, context_type_ *context) { \
        if (token == NULL) return SALTS_EINVAL; \
        if (token->active) return token->owner_address == token ? SALTS_EBUSY : SALTS_EINVAL; \
        if (!native_io_request_valid(request) || !native_io_endpoint_valid(endpoint) || \
            context == NULL) return SALTS_EINVAL; \
        token->request = request; \
        token->endpoint = endpoint; \
        token->correlation = correlation; \
        token->context = context; \
        token->owner_address = token; \
        token->active = true; \
        return SALTS_OK; \
    } \
    static inline int name_##_settle( \
        name_ *token, const native_io_completion *completion, \
        context_type_ **out_context) { \
        name_ empty = {0}; \
        if (out_context == NULL || token == NULL || completion == NULL) \
            return SALTS_EINVAL; \
        *out_context = NULL; \
        if (!token->active) return SALTS_EALREADY; \
        if (token->owner_address != token) return SALTS_EINVAL; \
        if (completion->request.slot != token->request.slot || \
            completion->request.generation != token->request.generation || \
            completion->endpoint.slot != token->endpoint.slot || \
            completion->endpoint.generation != token->endpoint.generation || \
            completion->user_data != token->correlation) return SALTS_ENOENT; \
        if (completion->kind != NATIVE_IO_COMPLETION_OK && \
            completion->kind != NATIVE_IO_COMPLETION_EOF && \
            completion->kind != NATIVE_IO_COMPLETION_CANCELLED && \
            completion->kind != NATIVE_IO_COMPLETION_FAILED) return SALTS_EINVAL; \
        *out_context = token->context; \
        *token = empty; \
        return SALTS_OK; \
    } \
    typedef int name_##_declaration_complete

#endif /* SALTS_NATIVE_IO_ACE_TOKEN_H */
