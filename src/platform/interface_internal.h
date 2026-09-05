#ifndef RNS_INTERFACE_INTERNAL_H
#define RNS_INTERFACE_INTERNAL_H
#include "reticulum/interface.h"
/* Atomically starts and claims an unstarted provider for its runtime owner. */
rns_status_t rns_interface_claim(rns_interface_t *interface_value);
#endif
