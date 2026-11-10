#define TINYMOCK_GENERATE_FUNCTION_OVERRIDES 1
#include "tinymock.h"
#include "tinymock_reflect_cases.h"

FunctionDeclAsAbi(stateful, void, &cmeta_type_void, CMETA_ABI_VOID, match_send,
    (MatchRequest, request, CMETA_PARAM_IN, &match_request_snapshot_type, CMETA_ABI_AGGREGATE));
TINYMOCk_FUNCTION_DECLARE(match_send);

#define MATCH_SENDER_METHODS(X, I) \
    X(I, FV1, void, send, stateful, &cmeta_type_void, CMETA_ABI_VOID, \
        (MatchRequest, request, CMETA_PARAM_IN, &match_request_snapshot_type, CMETA_ABI_AGGREGATE))
CMETA_INTERFACE(MatchSender, MATCH_SENDER_METHODS);
TINYMOCk_INTERFACE(MatchSender, MATCH_SENDER_METHODS);

suite("TinyMock generated reflection match facades") {
    static tinymock_MatchSender mock;
    before_each() {
        match_request_snapshot_type = *cmeta_reflected_storage(MatchRequest);
        match_request_snapshot_type.traits = &match_request_traits;
        TINYMOCk_FUNCTION_RESET(match_send);
        tinymock_MatchSender_init(&mock);
    }
    after_each() {
        TINYMOCk_FUNCTION_DESTROY(match_send);
        tinymock_MatchSender_destroy(&mock);
    }
    it("matches both exact free functions and interface methods") {
        MatchRequest request = {{true, 42}, 1.0};
        tinymock_reflect_result result;
        MatchSender sender = tinymock_MatchSender_as_interface(&mock);
        match_send(request);
        MatchSender_send(&sender, request);
        check_equal(TINYMOCk_FUNCTION_ARG_MATCH_DATA(match_send, 0u, "request",
            cmeta_reflected_data(MatchRequest), request, &result), CMETA_OK);
        check_true(result.equal);
        request.user.id = 7;
        check_equal(TINYMOCk_INTERFACE_ARG_MATCH_DATA(&mock, send, 0u, "request",
            cmeta_reflected_data(MatchRequest), request, &result), CMETA_OK);
        check_false(result.equal);
        check_equal(result.path, "request.user.id");
    }
}
