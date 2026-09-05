/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "packet_messaging.h"
#include "button_menu.h"
#include "live_view.h"
#include "home_view.h"
#include "cpu_usage.h"
#include "channel_view.h"
#include "chat_store.h"
#include "chat_admission.h"
#include "chat_journal.h"
#include "chat_view.h"
#include "archive_view.h"
#include "radio_discovery.h"
#include "reticulum/boards/heltec_reticulum_radio.h"
#include "reticulum/boards/heltec_status_ui_esp.h"
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/hal.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
static const char *TAG = "messaging";
static heltec_radio_discovery discovery;
static lxmf_packet_node_t *node;
static rns_heltec_oled_esp_t *display;
static uint64_t tx_done, tx_failed, preview_until;
static heltec_button_menu menu;
static heltec_live_view live;
static heltec_menu_action active_view;
static heltec_cpu_usage cpu;
static heltec_channel_view channel;
static heltec_chat_store *saved_chats;
static rns_storage_t *chat_storage;
static rns_status_t chat_status = RNS_ERROR_INVALID_STATE;
static heltec_chat_view chats_ui;
static heltec_message_archive *archive;
static heltec_archive_view archive_ui;
/* Shared scratch belongs to the single polling task, never an ISR. */
static heltec_archived_message archive_item;
static size_t archive_cursor = 64;
static uint64_t archive_generation, archive_next;
static bool archive_rescan;
static bool outbox_ready;
static rns_status_t quick_reply(void *context,const uint8_t sender[16],const char *text) {
    (void)context;
    uint8_t id[32]; uint64_t now=(uint64_t)esp_timer_get_time()/1000000U;
    uint64_t timestamp=(uint64_t)HELTEC_BUILD_EPOCH+now;
    /* Avoid identical reply IDs when uptime restarts without a wall clock.
     * Only local outgoing timestamps are trusted to advance this floor. */
    for(size_t i=0;i<8;++i) {
        const heltec_chat *chat=heltec_chat_store_get(saved_chats,i);
        if(!chat) continue;
        for(size_t j=0;j<chat->count;++j) if(chat->messages[j].state && chat->messages[j].timestamp>=timestamp) {
            if(chat->messages[j].timestamp==UINT64_MAX) return RNS_ERROR_OVERFLOW;
            timestamp=chat->messages[j].timestamp+1U;
        }
    }
    for(size_t i=0;i<4;++i) {
        lxmf_packet_outgoing out;
        if(lxmf_packet_node_outgoing(node,i,&out) && out.timestamp>=timestamp) {
            if(out.timestamp==UINT64_MAX) return RNS_ERROR_OVERFLOW;
            timestamp=out.timestamp+1U;
        }
    }
    return lxmf_packet_node_send(node,sender,(const uint8_t *)text,strlen(text),
        timestamp,id);
}
static rns_status_t cancel_reply(void *context,const uint8_t id[32]) {
    (void)context; return lxmf_packet_node_cancel(node,id);
}
static bool reply_delivery_line(void *context,const uint8_t id[32],char line[22]) {
    (void)context;
    static const char *names[]={"", "QUEUED", "TRANSMITTING", "AWAITING PROOF", "DELIVERED", "FAILED", "CANCELLED"};
    for(size_t i=0;i<4;++i) {
        lxmf_packet_outgoing out;
        if(lxmf_packet_node_outgoing(node,i,&out) && !memcmp(out.id,id,32)) {
            (void)snprintf(line,22,"%s %u/3",out.durable?names[out.state]:"SAVING STATE",out.attempts);
            return true;
        }
    }
    return false;
}
static bool save_outgoing_history(void) {
    if(!saved_chats || !outbox_ready) return false;
    for(size_t i=0;i<4;++i) {
        lxmf_packet_outgoing out;
        if(!lxmf_packet_node_outgoing(node,i,&out)) continue;
        /* Do not present unpersisted terminal transitions as durable delivery. */
        if(!out.durable) continue;
        uint8_t state=out.state==LXMF_PACKET_DELIVERED?3:
            out.state==LXMF_PACKET_FAILED?4:out.state==LXMF_PACKET_CANCELLED?5:
            out.state==LXMF_PACKET_AWAITING_PROOF?2:1;
        heltec_chat_message m={.timestamp=out.timestamp,.length=(uint16_t)out.text_length,.state=state};
        memcpy(m.id,out.id,32); memcpy(m.text,out.text,out.text_length);
        rns_status_t status=heltec_chat_store_add(saved_chats,out.destination,&m);
        if(status==RNS_OK) status=heltec_chat_store_set_state(saved_chats,out.destination,out.id,state);
        if(status==RNS_OK && out.state>=LXMF_PACKET_DELIVERED) status=lxmf_packet_node_release(node,i);
        rns_hal_secure_zero(&m,sizeof(m)); rns_hal_secure_zero(&out,sizeof(out));
        if(status!=RNS_OK) { chat_status=status; return false; }
    }
    return true;
}
static bool archive_has_id(const uint8_t id[32]) {
    for(size_t i=0;i<64;++i) {
        const heltec_archived_message *m=heltec_message_archive_get(archive,i);
        if(m && !memcmp(m->id,id,32)) return true;
    }
    return false;
}
static rns_status_t persist_message(void *context, const lxmf_message_t *message,
    lxmf_signature_state_t signature, const uint8_t *packet, size_t packet_length) {
    (void)context;
    if (!saved_chats || !archive) return chat_status;
    if (signature != LXMF_SIGNATURE_VERIFIED && signature != LXMF_SIGNATURE_UNVERIFIED)
        return RNS_ERROR_PROTOCOL;
    if (message->content.len > HELTEC_CHAT_TEXT || !isfinite(message->timestamp) ||
        message->timestamp < 0 || message->timestamp >= 18446744073709551616.0)
        return chat_status = RNS_ERROR_OVERFLOW;
    if(!heltec_chat_admission_available(saved_chats,archive,message,signature==LXMF_SIGNATURE_VERIFIED) || !packet || !packet_length || packet_length>500)
        return chat_status = RNS_ERROR_OVERFLOW;
    if(signature==LXMF_SIGNATURE_UNVERIFIED || archive_has_id(message->message_id)) {
        memset(&archive_item,0,sizeof(archive_item));
        archive_item.signature=signature; archive_item.received=(uint64_t)message->timestamp;
        archive_item.text_length=(uint16_t)message->content.len;
        archive_item.packet_length=(uint16_t)packet_length;
        memcpy(archive_item.source,message->source,16); memcpy(archive_item.id,message->message_id,32);
        if(message->content.len) memcpy(archive_item.text,message->content.data,message->content.len);
        memcpy(archive_item.packet,packet,packet_length);
        /* Keep an unknown record unknown until the verified chat save succeeds. */
        if(signature==LXMF_SIGNATURE_UNVERIFIED) {
            chat_status=heltec_message_archive_put(archive,&archive_item);
            rns_hal_secure_zero(&archive_item,sizeof(archive_item));
            return chat_status;
        }
    }
    heltec_chat_message item = {.timestamp = (uint64_t)message->timestamp,
        .length = (uint16_t)message->content.len, .state = 0};
    memcpy(item.id, message->message_id, 32);
    if (item.length) memcpy(item.text, message->content.data, item.length);
    chat_status = heltec_chat_store_add(saved_chats, message->source, &item);
    if(chat_status==RNS_OK && archive_has_id(message->message_id)) {
        /* The verified legacy record is durable first. Remove its quarantine
         * copy so later confirmed chat deletion does not leave hidden text. */
        for(size_t i=0;i<64;++i) {
            const heltec_archived_message *m=heltec_message_archive_get(archive,i);
            if(m && !memcmp(m->id,message->message_id,32)) {
                chat_status=heltec_message_archive_remove(archive,i); break;
            }
        }
    }
    rns_hal_secure_zero(&archive_item,sizeof(archive_item));
    rns_hal_secure_zero(&item, sizeof(item));
    return chat_status;
}
static void sample_cpu(void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    static TaskStatus_t tasks[24];
    configRUN_TIME_COUNTER_TYPE total = 0;
    uint32_t idle[2] = {0}; bool found[2] = {false, false};
    UBaseType_t count = uxTaskGetSystemState(tasks, 24U, &total);
    for (unsigned core = 0; core < 2U; ++core) {
        TaskHandle_t handle = xTaskGetIdleTaskHandleForCore((BaseType_t)core);
        for (UBaseType_t i = 0; i < count; ++i) if (tasks[i].xHandle == handle) {
            idle[core] = (uint32_t)tasks[i].ulRunTimeCounter; found[core] = true;
        }
    }
    heltec_cpu_sample(&cpu, (uint32_t)total, idle, count && found[0] && found[1]);
#else
    uint32_t idle[2] = {0}; heltec_cpu_sample(&cpu, 0, idle, false);
#endif
}
static uint64_t clock_ms(void *context) { (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static rns_status_t entropy(void *context, uint8_t *out, size_t size) {
    (void)context; return rns_hal_random_bytes(out, size);
}
static void tx_result(void *context, uint32_t id, rns_sx1262_packet_outcome_t outcome, rns_status_t status) {
    (void)context;
    lxmf_packet_node_tx_complete(node,id,outcome==RNS_SX1262_PACKET_SENT?RNS_OK:
        status==RNS_OK?RNS_ERROR_IO:status,clock_ms(NULL));
    if (outcome == RNS_SX1262_PACKET_SENT) ++tx_done; else ++tx_failed;
    ESP_LOGI(TAG, "RF completion outcome=%d status=%d (not a message delivery receipt)", (int)outcome, (int)status);
}
static void incoming_message(void *context, const lxmf_message_t *message) {
    (void)context;
    ESP_LOGI(TAG, "Verified short LXMF received; content bytes=%u", (unsigned)message->content.len);
    heltec_live_message(&live, message->content.data, message->content.len);
    if (display) {
        rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
        rns_heltec_oled_settings_t settings = oled->settings;
        settings.preview_timeout_ms = 30000U;
        rns_heltec_oled_set_settings(oled, &settings);
        (void)rns_heltec_oled_show_preview(oled, message->content.data, message->content.len, clock_ms(NULL));
        if (oled->settings.preview_enabled) {
            settings.screen = RNS_HELTEC_OLED_SCREEN_MESSAGE;
            rns_heltec_oled_set_settings(oled, &settings);
            preview_until = clock_ms(NULL) + 30000U;
        }
    }
}
static rns_status_t received(void *context, const uint8_t *packet, size_t length) {
    (void)context;
    heltec_radio_discovery_packet(&discovery, packet, length, clock_ms(NULL));
    /* Malformed or unsupported packets must not stop the radio poll loop. */
    (void)lxmf_packet_node_receive(node, packet, length);
    return RNS_OK;
}
void heltec_packet_messaging_run(rns_storage_t *storage) {
    rns_interface_t *radio = NULL;
    rns_heltec_reticulum_radio_config_t config;
    static const rns_sx1262_clock_ops_t clocks = {.monotonic_ms = clock_ms, .entropy = entropy};
    heltec_radio_discovery_init(&discovery);
    rns_heltec_reticulum_radio_default_config(&config);
    config.frequency_hz = 868100000U;
    config.scheduler.bandwidth_hz = 250000U;
    config.scheduler.spreading_factor = 11U;
    config.scheduler.coding_rate_denominator = 5U;
    config.scheduler.preamble_symbols = 18U;
    config.scheduler.duty_cycle_ppm = 10000U;
    config.tx_power_dbm = 14;
    rns_status_t status = rns_heltec_reticulum_radio_create(&config, &clocks, NULL, tx_result, NULL, &radio);
    ESP_LOGI(TAG, "Radio provider creation: %d", (int)status);
    if (status == RNS_OK) status = lxmf_packet_node_create(storage, radio, incoming_message, NULL, &node);
    ESP_LOGI(TAG, "Packet identity opening: %d", (int)status);
    if (status == RNS_OK) {
        chat_status = heltec_chat_flash_open(&chat_storage);
        if (chat_status == RNS_OK) chat_status = heltec_chat_store_open(chat_storage, &saved_chats);
        if (chat_status == RNS_OK) chat_status = heltec_message_archive_open(chat_storage, &archive);
        if (chat_status == RNS_OK) {
            chat_status=lxmf_packet_node_open_outbox(node,chat_storage);
            outbox_ready=chat_status==RNS_OK;
            if(outbox_ready) {
                chats_ui.send_reply=quick_reply; chats_ui.cancel_reply=cancel_reply;
                chats_ui.delivery_line=reply_delivery_line;
            }
        }
        if (chat_status == RNS_OK) {
            for (size_t slot = 0; slot < HELTEC_CHAT_COUNT; ++slot) {
                const heltec_chat *chat = heltec_chat_store_get(saved_chats, slot);
                if (!chat) continue;
                for (size_t i = chat->count; i-- > 0;)
                    heltec_live_message(&live, chat->messages[i].text, chat->messages[i].length);
            }
        }
        lxmf_packet_node_set_accept(node, persist_message, NULL);
        ESP_LOGI(TAG, "Chat storage opening: %d; identity storage unchanged", (int)chat_status);
        if(heltec_chat_flash_quarantined()) ESP_LOGW(TAG,"Storage degraded: quarantined records preserved");
    }
    if (status == RNS_OK) status = rns_interface_start(radio);
    if (status != RNS_OK) {
        ESP_LOGE(TAG, "Packet-mode startup failed: %d; storage not erased", (int)status);
        lxmf_packet_node_destroy(node); node = NULL;
        heltec_chat_store_close(saved_chats); saved_chats = NULL;
        heltec_message_archive_close(archive); archive = NULL;
        if (chat_storage) { rns_storage_destroy(chat_storage); chat_storage = NULL; }
        if (radio) rns_interface_destroy(radio);
        return;
    }
    (void)rns_heltec_oled_esp_open(&display);
    if (display) {
        rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
        rns_heltec_oled_settings_t settings = oled->settings;
        settings.preview_timeout_ms = 30000U;
        rns_heltec_oled_set_settings(oled, &settings);
    }
    gpio_config_t input = {.pin_bit_mask = UINT64_C(1) << RNS_HELTEC_V3_1_GPIO_PRG,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    bool button_ready = gpio_config(&input) == ESP_OK;
    ESP_LOGI(TAG, "868.100 MHz SF11 BW250 CR4/5; packet LXMF; PRG=%s; no automatic announce",
        button_ready ? "ready" : "unavailable");
    uint64_t next_display = 0, next_log = 0, next_cpu = 0;
    for (;;) {
        uint64_t now = clock_ms(NULL);
        if (now >= next_cpu) { sample_cpu(); next_cpu = now+1000U; }
        status = rns_interface_poll(radio, received, NULL, 4U);
        if(save_outgoing_history()) lxmf_packet_node_poll(node,now);
        lxmf_packet_node_stats_t archive_stats;
        lxmf_packet_node_stats(node,&archive_stats);
        if(archive_generation!=archive_stats.learned_announces) {
            archive_generation=archive_stats.learned_announces;
            if(archive_cursor==64) archive_cursor=0;
            else archive_rescan=true;
        }
        /* At most one archived packet per interval; no unbounded crypto scan. */
        if(archive && archive_cursor<64 && now>=archive_next) {
            const heltec_archived_message *m=heltec_message_archive_get(archive,archive_cursor++);
            archive_next=now+250U;
            if(m && m->signature==LXMF_SIGNATURE_UNVERIFIED) {
                lxmf_signature_state_t signature=LXMF_SIGNATURE_UNVERIFIED;
                if(lxmf_packet_node_check_archive(node,m->packet,m->packet_length,m->source,m->id,&signature)==RNS_OK) {
                    if(signature==LXMF_SIGNATURE_VERIFIED) {
                        /* Copy: the acceptance callback can replace this record. */
                        uint8_t raw[500]; size_t length=m->packet_length;
                        memcpy(raw,m->packet,length);
                        (void)lxmf_packet_node_receive(node,raw,length);
                        rns_hal_secure_zero(raw,sizeof(raw));
                    } else if(signature==LXMF_SIGNATURE_FAILED) {
                        archive_item=*m; archive_item.signature=signature;
                        chat_status=heltec_message_archive_put(archive,&archive_item);
                        rns_hal_secure_zero(&archive_item,sizeof(archive_item));
                    }
                }
            }
            if(archive_cursor==64 && archive_rescan) { archive_cursor=0; archive_rescan=false; }
        }
        rns_interface_stats_t radio_stats = {0};
        (void)rns_interface_get_stats(radio, &radio_stats);
        heltec_channel_sample(&channel, now, &radio_stats);
        heltec_radio_discovery_poll(&discovery, now);
        bool was_open = menu.open;
        uint8_t was_selected = menu.selected;
        heltec_menu_action action = button_ready ? heltec_button_menu_poll(&menu,
            gpio_get_level(RNS_HELTEC_V3_1_GPIO_PRG) == 0, now) : HELTEC_MENU_NONE;
        if (menu.open != was_open || menu.selected != was_selected || action != HELTEC_MENU_NONE)
            next_display = 0;
        if (menu.open) { preview_until = 0U; active_view = HELTEC_MENU_NONE; menu.browsing = false; menu.hold_action = false; }
        if (action == HELTEC_MENU_ANNOUNCE) {
            rns_status_t announced = lxmf_packet_node_announce(node, (uint64_t)HELTEC_BUILD_EPOCH + now / 1000U);
            ESP_LOGI(TAG, "PRG announce queue status=%d; airtime/CAD scheduling applies", (int)announced);
        }
        if (action == HELTEC_MENU_MESSAGE || action == HELTEC_MENU_NODES || action == HELTEC_MENU_CHANNEL || action == HELTEC_MENU_UNVERIFIED) {
            active_view = action; menu.browsing = true; next_display = 0;
            menu.hold_action = action == HELTEC_MENU_MESSAGE || action == HELTEC_MENU_UNVERIFIED;
        }
        if (action == HELTEC_MENU_CLEAR) {
            rns_hal_secure_zero(&live, sizeof(live));
            preview_until = 0U;
            if (display) {
                rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
                rns_hal_secure_zero(oled->model.preview, sizeof(oled->model.preview));
                oled->preview_deadline_ms = 0U;
            }
        }
        lxmf_packet_node_stats_t messages;
        lxmf_packet_node_stats(node, &messages);
        if (action == HELTEC_MENU_NEXT) next_display = 0;
        if (now >= next_display && display) {
            rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
            rns_heltec_oled_poll(oled, now);
            if (menu.open) rns_heltec_oled_set_menu(oled, heltec_button_menu_label(&menu));
            else if (active_view == HELTEC_MENU_CHANNEL) {
                char lines[8][22];
                heltec_channel_lines(&channel, &radio_stats, lines);
                rns_heltec_oled_set_lines(oled, (const char (*)[22])lines);
            }
            else if (active_view == HELTEC_MENU_MESSAGE || active_view == HELTEC_MENU_NODES || active_view == HELTEC_MENU_UNVERIFIED) {
                char lines[8][22];
                if (active_view == HELTEC_MENU_MESSAGE) {
                    if (heltec_chat_view_poll(&chats_ui, saved_chats, action == HELTEC_MENU_NEXT,
                        action == HELTEC_MENU_SELECT, lines)) {
                        menu.open = true; menu.hold_action = false;
                    }
                }
                else if(active_view==HELTEC_MENU_UNVERIFIED) {
                    if(heltec_archive_view_poll(&archive_ui,archive,action==HELTEC_MENU_NEXT,action==HELTEC_MENU_SELECT,lines)) {
                        menu.open=true; menu.hold_action=false;
                    }
                    if(archive_ui.deleted) {
                        lxmf_packet_node_forget_pending(node,archive_ui.deleted_id);
                        archive_ui.deleted=false;
                    }
                }
                else heltec_live_nodes(&live, &discovery, now, action == HELTEC_MENU_NEXT, lines);
                if (active_view == HELTEC_MENU_MESSAGE && chat_status != RNS_OK)
                    (void)snprintf(lines[1], 22, "STORAGE ERROR %d", (int)chat_status);
                if (menu.open) rns_heltec_oled_set_menu(oled, heltec_button_menu_label(&menu));
                else rns_heltec_oled_set_lines(oled, (const char (*)[22])lines);
            }
            else if (now >= preview_until) {
                heltec_home_snapshot snapshot = {.rx_packets = discovery.packets, .tx_packets = tx_done,
                    .heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    .heap_minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                    .cpu_valid = cpu.valid, .cpu_percent = cpu.percent};
                snapshot.radio_valid = rns_interface_get_stats(radio, &snapshot.radio) == RNS_OK;
                char lines[8][22];
                heltec_home_lines(&snapshot, lines);
                if (chat_status != RNS_OK) (void)snprintf(lines[7], 22, "STORAGE ERROR %d", (int)chat_status);
                rns_heltec_oled_set_lines(oled, (const char (*)[22])lines);
            }
            if(heltec_chat_flash_quarantined() && oled->settings.screen==RNS_HELTEC_OLED_SCREEN_LIVE) {
                (void)snprintf(oled->model.lines[7],22,"STORAGE DEGRADED"); oled->dirty=true;
            }
            if (!rns_heltec_oled_render(oled)) {
                rns_heltec_oled_esp_close(display); display = NULL;
                ESP_LOGE(TAG, "OLED offline; radio continues");
            }
            next_display = now + (active_view == HELTEC_MENU_CHANNEL && !menu.open ? 1000U : 250U);
        }
        if (now >= next_log) {
            ESP_LOGI(TAG, "Ingress=%" PRIu64 " malformed=%" PRIu64 " ifac=%" PRIu64
                " types(data,announce,link,proof)=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                " learned=%" PRIu64 " other_dest=%" PRIu64 " local_data=%" PRIu64
                " local_other=%" PRIu64 " unsupported_layout=%" PRIu64,
                messages.ingress, messages.malformed, messages.ifac_rejected,
                messages.packet_types[0], messages.packet_types[1], messages.packet_types[2], messages.packet_types[3],
                messages.learned_announces, messages.other_destinations, messages.local_data,
                messages.local_other, messages.unsupported_data_layout);
            ESP_LOGI(TAG, "RX=%" PRIu64 " TX=%" PRIu64 " TXfail=%" PRIu64
                " messages=%" PRIu64 " rejected=%" PRIu64 " unknown=%" PRIu64
                " pending=%" PRIu64 " unsupported_links=%" PRIu64
                " proofs=%" PRIu64 " last_message=%d poll=%d heap=%lu stack=%lu",
                discovery.packets, tx_done, tx_failed, messages.messages, messages.rejected,
                messages.unknown_senders, messages.pending_senders, messages.unsupported_packets, messages.proofs_queued, (int)messages.last_message_status,
                (int)status, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            next_log = now + 10000U;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
}
