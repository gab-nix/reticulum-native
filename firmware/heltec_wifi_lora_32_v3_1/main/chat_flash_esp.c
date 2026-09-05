/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_journal.h"
#include "esp_partition.h"
#include <stdlib.h>
static rns_status_t rd(void *c,size_t o,uint8_t *p,size_t n) {
    if(o>HELTEC_CHAT_JOURNAL_BYTES || n>HELTEC_CHAT_JOURNAL_BYTES-o) return RNS_ERROR_OVERFLOW;
    return esp_partition_read(c,o,p,n)==ESP_OK?RNS_OK:RNS_ERROR_IO;
}
static rns_status_t erase(void *c,size_t o,size_t n) {
    if(o>HELTEC_CHAT_JOURNAL_BYTES || n>HELTEC_CHAT_JOURNAL_BYTES-o) return RNS_ERROR_OVERFLOW;
    return esp_partition_erase_range(c,o,n)==ESP_OK?RNS_OK:RNS_ERROR_IO;
}
static rns_status_t wr(void *c,size_t o,const uint8_t *p,size_t n) {
    if(o>HELTEC_CHAT_JOURNAL_BYTES || n>HELTEC_CHAT_JOURNAL_BYTES-o) return RNS_ERROR_OVERFLOW;
    return esp_partition_write(c,o,p,n)==ESP_OK?RNS_OK:RNS_ERROR_IO;
}
rns_status_t heltec_chat_flash_open(rns_storage_t **out) {
    if(!out) return RNS_ERROR_INVALID_ARGUMENT;
    *out=NULL;
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"storage");
    if(!p || p->size<HELTEC_CHAT_JOURNAL_BYTES) return RNS_ERROR_NOT_FOUND;
    /* This reserved partition must not contain an existing filesystem or
     * foreign data. Never format it or change its partition boundaries. */
    uint8_t *buffer=malloc(4096); if(!buffer) return RNS_ERROR_NO_MEMORY;
    for(size_t offset=HELTEC_CHAT_JOURNAL_BYTES;offset<p->size;offset+=4096U) {
        size_t n=p->size-offset<4096U?p->size-offset:4096U;
        if(esp_partition_read(p,offset,buffer,n)!=ESP_OK) { free(buffer); return RNS_ERROR_IO; }
        for(size_t i=0;i<n;++i) if(buffer[i]!=255U) { free(buffer); return RNS_ERROR_PROTOCOL; }
    }
    free(buffer);
    heltec_chat_flash_ops ops={.context=(void *)p,.read=rd,.erase=erase,.write=wr};
    return heltec_chat_journal_open(&ops,out);
}
