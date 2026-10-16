#include "installed_dso_scenario.h"
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error Exact C11 is required for the installed SDK DSO consumer
#endif
_Static_assert(SALTS_SERVICE_HOST_MAX_ATTACHED_GENERATIONS == 2u,
               "ServiceHost must retain the two-generation bound");
int main(int argc, char **argv) { return installed_dso_run(argc, argv); }
