#include <stdio.h>
#include "probe.h"

int main(void)
{
    printf("salts-turbo-build-probe:%d\n", salts_turbo_build_probe_value());
    return 0;
}

int salts_turbo_build_probe_value(void)
{
    return 23 * SALTS_TURBO_BUILD_PROBE_MULTIPLIER;
}
