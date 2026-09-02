#include "reticulum/runtime.h"

#include <assert.h>
#include <string.h>

int main(void) {
    rns_config_t config;
    rns_runtime_t *runtime = NULL;
    rns_runtime_interface_info_t info;
    rns_runtime_link_t *link = (rns_runtime_link_t *)1;
    rns_identity remote;
    uint8_t destination[16] = {1U};
    size_t processed = 99U;

    rns_config_init(&config);
    config.interface_count = 2U;
    (void)strcpy(config.interfaces[0].name, "disabled UDP");
    config.interfaces[0].type = RNS_CONFIG_UDP;
    config.interfaces[0].type_set = true;
    (void)strcpy(config.interfaces[1].name, "radio placeholder");
    config.interfaces[1].type = RNS_CONFIG_RNODE;
    config.interfaces[1].type_set = true;
    config.interfaces[1].enabled = true;

    assert(rns_runtime_create(NULL, &config, NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    assert(rns_identity_generate(&remote));
    assert(rns_runtime_link_open(runtime, destination, &remote, NULL, &link) ==
           RNS_ERROR_NOT_FOUND);
    assert(link == NULL);
    assert(rns_runtime_link_state(NULL) == RNS_LINK_CLOSED);
    assert(rns_runtime_link_id(NULL) == NULL);
    assert(runtime != NULL);
    assert(rns_runtime_interface_count(runtime) == 2U);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(strcmp(info.name, "disabled UDP") == 0);
    assert(info.state == RNS_RUNTIME_INTERFACE_DISABLED);
    assert(rns_runtime_interface_info(runtime, 1U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UNSUPPORTED);
    assert(info.last_error == RNS_ERROR_UNSUPPORTED);
    assert(rns_runtime_interface_info(runtime, 2U, &info) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_register_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_register_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_unregister_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_unregister_destination(runtime, destination) == RNS_ERROR_NOT_FOUND);
    assert(rns_runtime_request_path(runtime, destination) == RNS_ERROR_INVALID_STATE);
    assert(rns_runtime_path_lookup(runtime, destination, &(rns_path_entry){0}) == RNS_ERROR_NOT_FOUND);
    assert(rns_runtime_path_snapshot(runtime, NULL, 0U) == 0U);
    assert(rns_runtime_poll(runtime, 0U, &processed) == RNS_OK);
    assert(processed == 0U);
    /* A disabled interface has no startup error, but it cannot send. */
    assert(rns_runtime_send(runtime, 0U, destination, sizeof(destination)) ==
           RNS_ERROR_INVALID_STATE);
    rns_runtime_destroy(runtime);

    config.panic_on_interface_error = true;
    runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_ERROR_UNSUPPORTED);
    assert(runtime == NULL);
    return 0;
}
