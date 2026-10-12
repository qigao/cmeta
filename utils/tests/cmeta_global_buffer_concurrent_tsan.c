/* Salts #1170: the REAL mem_global() allocator, not a copied pool.
 * This is a Linux-only, first-party source-TSan qualification target.
 * All pool/cache/refcount synchronization must be visible to TSan; do
 * not replace the allocator or annotate away its ownership protocol. */
#define _POSIX_C_SOURCE 200809L
#include "cmeta_buffer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PRODUCERS=2, OVERLAP_ROUNDS=256, STRESS_ROUNDS=20000,
       OVERLAP_BYTES=3000 };
typedef struct test_state {
    pthread_barrier_t phase;
    mem_buffer_t *live[PRODUCERS];
    _Atomic unsigned failures;
} test_state;
typedef struct worker_state {
    test_state *state;
    unsigned lane;
} worker_state;

static void fail(test_state *state,const char *reason,unsigned lane,unsigned round) {
    fprintf(stderr,"global buffer ownership: %s lane=%u round=%u\n",
            reason,lane,round);
    atomic_fetch_add_explicit(&state->failures,1u,memory_order_relaxed);
}
static void phase_wait(test_state *state) {
    int rc=pthread_barrier_wait(&state->phase);
    if(rc!=0 && rc!=PTHREAD_BARRIER_SERIAL_THREAD)abort();
}
static uint8_t byte_at(unsigned lane,unsigned round) {
    return (uint8_t)(0x31u + 0x43u*lane + (round*17u)%127u);
}
static void *producer(void *arg) {
    worker_state *w=(worker_state *)arg;
    test_state *state=w->state;
    unsigned round;
    for(round=0;round<OVERLAP_ROUNDS;++round) {
        uint8_t expected=byte_at(w->lane,round);
        mem_buffer_t *b=mem_get_buffer(mem_global(),OVERLAP_BYTES);
        if(!b)fail(state,"overlap allocate failed",w->lane,round);
        state->live[w->lane]=b;
        phase_wait(state); /* Both original leases acquired. */
        phase_wait(state); /* Main verified no same live buffer. */
        if(b) {
            memset(mem_buffer_data(b),expected,OVERLAP_BYTES);
            mem_set_used(b,OVERLAP_BYTES);
        }
        phase_wait(state); /* Both writes have completed. */
        phase_wait(state); /* Main verified both byte patterns. */
        if(b)mem_buffer_release(b);
        state->live[w->lane]=NULL;
        phase_wait(state); /* All prior references dropped. */
    }

    /* Ungated independent producer activity forces legitimate cross-thread
     * recycling. A global mutex or round barrier here would hide missing
     * allocator release/acquire edges from fully source-instrumented TSan. */
    for(round=0;round<STRESS_ROUNDS;++round) {
        static const size_t sizes[]={64u,512u,3000u};
        size_t n=sizes[round%3u];
        uint8_t value=byte_at(w->lane,round);
        mem_buffer_t *b=mem_get_buffer(mem_global(),n);
        uint8_t *data;
        if(!b) {fail(state,"stress allocate failed",w->lane,round);continue;}
        data=(uint8_t *)mem_buffer_data(b);
        if(!data || mem_buffer_capacity(b)<n || mem_buffer_ref_count(b)!=1u) {
            fail(state,"invalid fresh lease",w->lane,round);
        } else {
            memset(data,value,n);
            mem_set_used(b,n);
            if(data[0]!=value || data[n/2u]!=value || data[n-1u]!=value)
                fail(state,"bytes mutated while lease active",w->lane,round);
        }
        mem_buffer_release(b);
    }
    return NULL;
}
int main(void) {
    test_state state={0};
    worker_state workers[PRODUCERS];
    pthread_t threads[PRODUCERS];
    unsigned round,i;
    if(pthread_barrier_init(&state.phase,NULL,PRODUCERS+1u)!=0)
        return 2;
    for(i=0;i<PRODUCERS;++i) {
        workers[i]=(worker_state){&state,i};
        if(pthread_create(&threads[i],NULL,producer,&workers[i])!=0)
            abort();
    }
    for(round=0;round<OVERLAP_ROUNDS;++round) {
        mem_buffer_t *a,*b;
        phase_wait(&state);
        a=state.live[0];b=state.live[1];
        if(a && b) {
            if(a==b || mem_buffer_data(a)==mem_buffer_data(b))
                fail(&state,"two LIVE leases alias",2u,round);
            if(mem_buffer_ref_count(a)!=1u || mem_buffer_ref_count(b)!=1u)
                fail(&state,"unexpected live refcount",2u,round);
        }
        phase_wait(&state);
        phase_wait(&state);
        for(i=0;i<PRODUCERS;++i) {
            mem_buffer_t *value=state.live[i];
            if(value) {
                uint8_t expected=byte_at(i,round);
                const uint8_t *data=(const uint8_t *)mem_buffer_const_data(value);
                if(mem_buffer_used(value)!=OVERLAP_BYTES ||
                   data[0]!=expected || data[OVERLAP_BYTES/2u]!=expected ||
                   data[OVERLAP_BYTES-1u]!=expected)
                    fail(&state,"live bytes corrupted",i,round);
            }
        }
        phase_wait(&state);
        phase_wait(&state);
    }
    for(i=0;i<PRODUCERS;++i)
        if(pthread_join(threads[i],NULL)!=0)abort();
    pthread_barrier_destroy(&state.phase);
    if(atomic_load_explicit(&state.failures,memory_order_relaxed)!=0u)
        return 1;
    puts("Salts #1170: exact global buffer leases, two active owners and 40000 ungated recycles OK");
    return 0;
}
