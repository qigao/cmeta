#ifndef CMETA_PLUGIN_REGISTRY_LIFETIME_ADAPTER_H
#define CMETA_PLUGIN_REGISTRY_LIFETIME_ADAPTER_H

/* One-shot scheduling seam in the existing Registry lifetime suite. Native
 * open/close and all Registry logic remain the production implementations. */
void cmeta_plugin_test_after_close(void (*callback)(void *), void *context);
void cmeta_plugin_test_fail_close_once(void);

#endif
