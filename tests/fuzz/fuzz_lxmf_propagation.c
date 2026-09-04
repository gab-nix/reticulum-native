#include "reticulum/lxmf_propagation.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t length);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t length) {
    if (length > LXMF_PN_MAX_WIRE) return 0;
    lxmf_pn_announce_t announce;
    lxmf_pn_upload_t upload;
    lxmf_pn_get_request_t request;
    lxmf_pn_get_response_t response;
    (void)lxmf_pn_announce_decode(data, length, &announce);
    (void)lxmf_pn_upload_decode(data, length, &upload);
    (void)lxmf_pn_get_request_decode(data, length, &request);
    (void)lxmf_pn_get_response_decode(data, length, true, &response);
    (void)lxmf_pn_get_response_decode(data, length, false, &response);
    (void)lxmf_pn_upload_rejection_decode(data, length);
    return 0;
}
