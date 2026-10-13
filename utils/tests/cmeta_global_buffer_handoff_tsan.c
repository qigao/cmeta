/* Salts #1170: REAL published/source mem_global cache shell handoff.
 *
 * Unlike a pthread barrier, the relaxed turn marker below establishes NO
 * C11 release/acquire happens-before between successive buffer payload
 * writes. The allocator's actual atomic magazine/cache publication MUST be
 * the only ownership synchronization between Owner 0 and Owner 1.
 *
 * The very same C11 file is linked into source-instrumented Salts for TSan
 * and built separately against the immutable published Salts SDK.
 * There is no copied allocator, mock return, suppression or global lock.
 */
#define _POSIX_C_SOURCE 200809L
#include <cmeta_buffer.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { HANDOFF_OWNERS = 2, HANDOFF_ROUNDS = 1024, HANDOFF_BYTES = 3000,
       HANDOFF_WAIT_YIELDS = 10000000 };

typedef struct handoff_state {
    _Atomic unsigned turn;            /* relaxed: ONLY execution sequencing */
    _Atomic uintptr_t original_data; /* relaxed: pointer identity, not a lease */
    _Atomic unsigned failures;
} handoff_state;
typedef struct handoff_actor {
    handoff_state *state;
    unsigned owner;
} handoff_actor;

static void fail(handoff_state *state, const char *why, unsigned owner,
                 unsigned round) {
    fprintf(stderr,"Salts #1170 handoff %s owner=%u epoch=%u\n",
            why,owner,round);
    atomic_fetch_add_explicit(&state->failures,1u,memory_order_relaxed);
}

static int await_turn(handoff_state *state, unsigned epoch) {
    unsigned count;
    for (count=0u; count<HANDOFF_WAIT_YIELDS; ++count) {
        if (atomic_load_explicit(&state->turn,memory_order_relaxed)==epoch)
            return 1;
        sched_yield();
    }
    return 0;
}

static void *handoff_actor_run(void *opaque) {
    handoff_actor *actor=(handoff_actor *)opaque;
    handoff_state *state=actor->state;
    unsigned epoch;
    for (epoch=actor->owner; epoch<HANDOFF_ROUNDS;
         epoch+=HANDOFF_OWNERS) {
        mem_buffer_t *buffer;
        uint8_t *bytes;
        uintptr_t first;
        uint8_t expected=(uint8_t)(0x20u+(epoch*37u)%191u);
        if (!await_turn(state,epoch)) {
            fail(state,"relaxed turn deadline exceeded",actor->owner,epoch);
            atomic_store_explicit(&state->turn,epoch+1u,memory_order_relaxed);
            return NULL;
        }
        buffer=mem_get_buffer(mem_global(),HANDOFF_BYTES);
        if (!buffer) {
            fail(state,"original allocator rejected lease",actor->owner,epoch);
            atomic_store_explicit(&state->turn,epoch+1u,memory_order_relaxed);
            continue;
        }
        bytes=(uint8_t *)mem_buffer_data(buffer);
        first=atomic_load_explicit(&state->original_data,memory_order_relaxed);
        if (epoch==0u) {
            if (!bytes) fail(state,"empty data pointer",actor->owner,epoch);
            atomic_store_explicit(&state->original_data,(uintptr_t)bytes,
                                  memory_order_relaxed);
        } else if (first==0u || (uintptr_t)bytes!=first) {
            /* The experiment must actually borrow the SAME released shell,
             * not merely run threads with independent per-shard caches. */
            fail(state,"original live shell was not recycled to other Owner",
                 actor->owner,epoch);
        }
        if (!bytes || mem_buffer_capacity(buffer)<HANDOFF_BYTES ||
            mem_buffer_ref_count(buffer)!=1u ||
            mem_buffer_used(buffer)!=0u) {
            fail(state,"fresh cross-Owner borrow contract broken",
                 actor->owner,epoch);
        } else {
            memset(bytes,expected,HANDOFF_BYTES);
            mem_set_used(buffer,HANDOFF_BYTES);
            if (bytes[0]!=expected || bytes[HANDOFF_BYTES/2u]!=expected ||
                bytes[HANDOFF_BYTES-1u]!=expected)
                fail(state,"buffer modified during exclusive lease",
                     actor->owner,epoch);
        }
        mem_buffer_release(buffer);
        /* Compiler ordering only; not an interthread acquire/release. */
        atomic_signal_fence(memory_order_seq_cst);
        atomic_store_explicit(&state->turn,epoch+1u,memory_order_relaxed);
    }
    return NULL;
}

int main(void) {
    handoff_state state={0};
    handoff_actor actors[HANDOFF_OWNERS];
    pthread_t threads[HANDOFF_OWNERS];
    unsigned i;
    for (i=0u;i<HANDOFF_OWNERS;++i) {
        actors[i]=(handoff_actor){&state,i};
        if (pthread_create(&threads[i],NULL,handoff_actor_run,&actors[i])!=0) {
            fprintf(stderr,"Salts #1170: could not start owner %u\n",i);
            return 2;
        }
    }
    for (i=0u;i<HANDOFF_OWNERS;++i) {
        if (pthread_join(threads[i],NULL)!=0) {
            fprintf(stderr,"Salts #1170: could not join owner %u\n",i);
            return 2;
        }
    }
    if (atomic_load_explicit(&state.failures,memory_order_relaxed)!=0u ||
        atomic_load_explicit(&state.turn,memory_order_relaxed)!=HANDOFF_ROUNDS ||
        atomic_load_explicit(&state.original_data,memory_order_relaxed)==0u)
        return 1;
    puts("Salts #1170: 1024 same-shell cross-owner relaxed turn handoffs OK");
    return 0;
}
