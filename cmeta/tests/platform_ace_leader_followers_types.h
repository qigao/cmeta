#ifndef SALTS_PLATFORM_ACE_LEADER_FOLLOWERS_TYPES_H
#define SALTS_PLATFORM_ACE_LEADER_FOLLOWERS_TYPES_H
/* Test-local typed CPU event contract. No installed Salts ABI or scheduler. */
enum lf_role { LF_NEW, LF_FOLLOWER, LF_LEADER, LF_PROCESSING, LF_STOPPED };
enum lf_result { LF_PENDING, LF_OK, LF_ERROR, LF_CANCELLED };
enum lf_stop { LF_RUN, LF_STOP_CANCEL, LF_STOP_FAILURE };
typedef enum lf_result (*lf_handler_fn)(void *context, int event);
#endif
