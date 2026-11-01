#include <cflow/clock.h>
#include <clock.h>

#include <stdlib.h>
#include <string.h>

typedef struct cflow_system_clock_state {
    int reserved;
} cflow_system_clock_state;

typedef struct cflow_virtual_clock_state {
    cflow_instant now;
} cflow_virtual_clock_state;

static cflow_instant system_clock_now(void *state) {
    (void)state;
    return (cflow_instant){cmeta_hrtime()};
}

static bool system_clock_advance(void *state, cflow_duration delta) {
    (void)state;
    (void)delta;
    return false;
}

static void system_clock_destroy(void *state) { free(state); }

CMETA_IMPLEMENTS(cflow_clock, system_clock,
    0,
    .now = system_clock_now,
    .advance = system_clock_advance,
    .destroy = system_clock_destroy
);

static cflow_instant virtual_clock_now(void *state) {
    cflow_virtual_clock_state *clock = (cflow_virtual_clock_state *)state;
    return clock ? clock->now : (cflow_instant){0};
}

static bool virtual_clock_advance(void *state, cflow_duration delta) {
    cflow_virtual_clock_state *clock = (cflow_virtual_clock_state *)state;
    if (!clock) return false;
    clock->now.ns = delta.ns > UINT64_MAX - clock->now.ns
                        ? UINT64_MAX
                        : clock->now.ns + delta.ns;
    return true;
}

static void virtual_clock_destroy(void *state) { free(state); }

CMETA_IMPLEMENTS(cflow_clock, virtual_clock,
    CMETA_CLOCK_CAP_MANUAL,
    .now = virtual_clock_now,
    .advance = virtual_clock_advance,
    .destroy = virtual_clock_destroy
);

bool cflow_clock_system_init(cflow_clock *clock) {
    cflow_system_clock_state *state;
    if (!clock) return false;
    memset(clock, 0, sizeof(*clock));
    state = (cflow_system_clock_state *)calloc(1, sizeof(*state));
    if (!state) return false;
    *clock = system_clock_as_cflow_clock(state);
    return true;
}

bool cflow_clock_virtual_init(cflow_clock *clock, cflow_instant start) {
    cflow_virtual_clock_state *state;
    if (!clock) return false;
    memset(clock, 0, sizeof(*clock));
    state = (cflow_virtual_clock_state *)calloc(1, sizeof(*state));
    if (!state) return false;
    state->now = start;
    *clock = virtual_clock_as_cflow_clock(state);
    return true;
}
