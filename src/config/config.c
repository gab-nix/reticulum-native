#include "reticulum/config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 512U

typedef enum parse_section {
    SECTION_NONE = 0,
    SECTION_RETICULUM,
    SECTION_INTERFACES,
    SECTION_INTERFACE
} parse_section_t;

static void set_diagnostic(rns_config_diagnostic_t *diagnostic,
                           size_t line,
                           rns_status_t status,
                           const char *format,
                           ...) {
    va_list arguments;
    if (diagnostic == NULL) {
        return;
    }
    diagnostic->line = line;
    diagnostic->status = status;
    va_start(arguments, format);
    (void)vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, arguments);
    va_end(arguments);
}

static char *trim(char *value) {
    char *end;
    while (*value != '\0' && isspace((unsigned char)*value) != 0) {
        ++value;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]) != 0) {
        --end;
    }
    *end = '\0';
    return value;
}

static bool copy_value(char *destination, size_t capacity, const char *source) {
    size_t length = strlen(source);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool equal_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool parse_bool(const char *value, bool *result) {
    if (equal_ignore_case(value, "yes") || equal_ignore_case(value, "true") ||
        equal_ignore_case(value, "on") || strcmp(value, "1") == 0) {
        *result = true;
        return true;
    }
    if (equal_ignore_case(value, "no") || equal_ignore_case(value, "false") ||
        equal_ignore_case(value, "off") || strcmp(value, "0") == 0) {
        *result = false;
        return true;
    }
    return false;
}

static bool parse_unsigned(const char *value, uint32_t maximum, uint32_t *result) {
    char *end = NULL;
    unsigned long parsed;
    if (*value == '\0' || *value == '-') {
        return false;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > maximum) {
        return false;
    }
    *result = (uint32_t)parsed;
    return true;
}

static bool parse_signed(const char *value, int32_t minimum, int32_t maximum, int32_t *result) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < (long)minimum || parsed > (long)maximum) {
        return false;
    }
    *result = (int32_t)parsed;
    return true;
}

void rns_config_init(rns_config_t *config) {
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->share_instance = true;
        config->shared_instance_type = RNS_CONFIG_SHARED_INSTANCE_TCP;
        (void)memcpy(config->instance_name, "default", sizeof("default"));
        config->shared_instance_port = 37428U;
        config->instance_control_port = 37429U;
        config->instance_data_port = 37428U;
    }
}

const char *rns_config_interface_type_name(rns_config_interface_type_t type) {
    switch (type) {
        case RNS_CONFIG_TCP_CLIENT: return "TCPClientInterface";
        case RNS_CONFIG_TCP_SERVER: return "TCPServerInterface";
        case RNS_CONFIG_UDP: return "UDPInterface";
        case RNS_CONFIG_AUTO: return "AutoInterface";
        case RNS_CONFIG_KISS: return "KISSInterface";
        case RNS_CONFIG_RNODE: return "RNodeInterface";
        default: return NULL;
    }
}

static bool parse_type(const char *value, rns_config_interface_type_t *type) {
    size_t candidate;
    for (candidate = 0U; candidate <= (size_t)RNS_CONFIG_RNODE; ++candidate) {
        const char *name = rns_config_interface_type_name((rns_config_interface_type_t)candidate);
        if (name != NULL && equal_ignore_case(value, name)) {
            *type = (rns_config_interface_type_t)candidate;
            return true;
        }
    }
    return false;
}

static rns_status_t parse_reticulum_value(rns_config_t *config,
                                          const char *key,
                                          const char *value,
                                          size_t line,
                                          rns_config_diagnostic_t *diagnostic) {
    uint32_t number;
    if (strcmp(key, "enable_transport") == 0) {
        if (parse_bool(value, &config->enable_transport)) return RNS_OK;
    } else if (strcmp(key, "share_instance") == 0) {
        if (parse_bool(value, &config->share_instance)) {
            config->share_instance_configured = true;
            return RNS_OK;
        }
    } else if (strcmp(key, "panic_on_interface_error") == 0) {
        if (parse_bool(value, &config->panic_on_interface_error)) return RNS_OK;
    } else if (strcmp(key, "shared_instance_type") == 0) {
        if (equal_ignore_case(value, "tcp")) {
            config->shared_instance_type = RNS_CONFIG_SHARED_INSTANCE_TCP;
            return RNS_OK;
        }
        if (equal_ignore_case(value, "unix")) {
            config->shared_instance_type = RNS_CONFIG_SHARED_INSTANCE_UNIX;
            return RNS_OK;
        }
    } else if (strcmp(key, "instance_name") == 0) {
        if (copy_value(config->instance_name, sizeof(config->instance_name), value))
            return RNS_OK;
    } else if (strcmp(key, "shared_instance_port") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number) && number != 0U) {
            config->shared_instance_port = (uint16_t)number;
            config->instance_data_port = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "instance_control_port") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number) && number != 0U) {
            config->instance_control_port = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "instance_data_port") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number) && number != 0U) {
            config->instance_data_port = (uint16_t)number;
            config->shared_instance_port = (uint16_t)number;
            return RNS_OK;
        }
    } else {
        set_diagnostic(diagnostic, line, RNS_ERROR_UNSUPPORTED,
                       "unsupported [reticulum] option '%s'", key);
        return RNS_ERROR_UNSUPPORTED;
    }
    set_diagnostic(diagnostic, line, RNS_ERROR_PROTOCOL,
                   "invalid value '%s' for [reticulum] option '%s'", value, key);
    return RNS_ERROR_PROTOCOL;
}

static rns_status_t parse_interface_value(rns_config_interface_t *interface,
                                          const char *key,
                                          const char *value,
                                          size_t line,
                                          rns_config_diagnostic_t *diagnostic) {
    uint32_t number;
    if (strcmp(key, "type") == 0) {
        if (!parse_type(value, &interface->type)) {
            set_diagnostic(diagnostic, line, RNS_ERROR_UNSUPPORTED,
                           "interface '%s' uses unsupported type '%s'", interface->name, value);
            return RNS_ERROR_UNSUPPORTED;
        }
        interface->type_set = true;
        return RNS_OK;
    }
    if (strcmp(key, "enabled") == 0) {
        if (parse_bool(value, &interface->enabled)) return RNS_OK;
    } else if (strcmp(key, "target_host") == 0) {
        if (copy_value(interface->target_host, sizeof(interface->target_host), value)) return RNS_OK;
    } else if (strcmp(key, "listen_ip") == 0) {
        if (copy_value(interface->listen_ip, sizeof(interface->listen_ip), value)) return RNS_OK;
    } else if (strcmp(key, "forward_ip") == 0) {
        if (copy_value(interface->forward_ip, sizeof(interface->forward_ip), value)) return RNS_OK;
    } else if (strcmp(key, "target_port") == 0 || strcmp(key, "listen_port") == 0 ||
               strcmp(key, "forward_port") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number) && number != 0U) {
            if (key[0] == 't') interface->target_port = (uint16_t)number;
            else if (key[0] == 'l') interface->listen_port = (uint16_t)number;
            else interface->forward_port = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "port") == 0) {
        if (copy_value(interface->device, sizeof(interface->device), value)) return RNS_OK;
    } else if (strcmp(key, "speed") == 0) {
        if (parse_unsigned(value, UINT32_MAX, &interface->speed) && interface->speed != 0U) return RNS_OK;
    } else if (strcmp(key, "preamble") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number)) {
            interface->preamble_ms = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "txtail") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number)) {
            interface->tx_tail_ms = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "slottime") == 0) {
        if (parse_unsigned(value, UINT16_MAX, &number)) {
            interface->slot_time_ms = (uint16_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "persistence") == 0) {
        if (parse_unsigned(value, UINT8_MAX, &number)) {
            interface->persistence = (uint8_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "flow_control") == 0) {
        if (parse_bool(value, &interface->flow_control)) return RNS_OK;
    } else if (strcmp(key, "databits") == 0) {
        if (parse_unsigned(value, 8U, &number) && number >= 5U) {
            interface->data_bits = (uint8_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "parity") == 0) {
        if (equal_ignore_case(value, "N") || equal_ignore_case(value, "none"))
            interface->parity = 'N';
        else if (equal_ignore_case(value, "E") ||
                 equal_ignore_case(value, "even"))
            interface->parity = 'E';
        else if (equal_ignore_case(value, "O") ||
                 equal_ignore_case(value, "odd"))
            interface->parity = 'O';
        else goto invalid_value;
        return RNS_OK;
    } else if (strcmp(key, "stopbits") == 0) {
        if (parse_unsigned(value, 2U, &number) && number >= 1U) {
            interface->stop_bits = (uint8_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "frequency") == 0) {
        if (parse_unsigned(value, UINT32_MAX, &interface->frequency)) return RNS_OK;
    } else if (strcmp(key, "bandwidth") == 0) {
        if (parse_unsigned(value, UINT32_MAX, &interface->bandwidth)) return RNS_OK;
    } else if (strcmp(key, "txpower") == 0 || strcmp(key, "tx_power") == 0) {
        if (parse_signed(value, -127, 127, &interface->tx_power)) return RNS_OK;
    } else if (strcmp(key, "spreadingfactor") == 0 || strcmp(key, "spreading_factor") == 0) {
        if (parse_unsigned(value, UINT8_MAX, &number)) {
            interface->spreading_factor = (uint8_t)number;
            return RNS_OK;
        }
    } else if (strcmp(key, "codingrate") == 0 || strcmp(key, "coding_rate") == 0) {
        if (parse_unsigned(value, UINT8_MAX, &number)) {
            interface->coding_rate = (uint8_t)number;
            return RNS_OK;
        }
    } else {
        set_diagnostic(diagnostic, line, RNS_ERROR_UNSUPPORTED,
                       "unsupported option '%s' in interface '%s'", key, interface->name);
        return RNS_ERROR_UNSUPPORTED;
    }
invalid_value:
    set_diagnostic(diagnostic, line, RNS_ERROR_PROTOCOL,
                   "invalid value '%s' for option '%s' in interface '%s'",
                   value, key, interface->name);
    return RNS_ERROR_PROTOCOL;
}

static rns_status_t validate_interface(const rns_config_interface_t *interface,
                                       rns_config_diagnostic_t *diagnostic) {
    if (!interface->type_set) {
        set_diagnostic(diagnostic, 0U, RNS_ERROR_PROTOCOL,
                       "interface '%s' has no type", interface->name);
        return RNS_ERROR_PROTOCOL;
    }
    if (!interface->enabled) {
        return RNS_OK;
    }
    if (interface->type == RNS_CONFIG_TCP_CLIENT &&
        (interface->target_host[0] == '\0' || interface->target_port == 0U)) {
        set_diagnostic(diagnostic, 0U, RNS_ERROR_PROTOCOL,
                       "enabled TCP client '%s' requires target_host and target_port", interface->name);
        return RNS_ERROR_PROTOCOL;
    }
    if (interface->type == RNS_CONFIG_TCP_SERVER && interface->listen_port == 0U) {
        set_diagnostic(diagnostic, 0U, RNS_ERROR_PROTOCOL,
                       "enabled TCP server '%s' requires listen_port", interface->name);
        return RNS_ERROR_PROTOCOL;
    }
    if (interface->type == RNS_CONFIG_UDP &&
        (interface->listen_port == 0U || interface->forward_port == 0U ||
         interface->forward_ip[0] == '\0')) {
        set_diagnostic(diagnostic, 0U, RNS_ERROR_PROTOCOL,
                       "enabled UDP interface '%s' requires listen_port, forward_ip and forward_port",
                       interface->name);
        return RNS_ERROR_PROTOCOL;
    }
    if ((interface->type == RNS_CONFIG_KISS || interface->type == RNS_CONFIG_RNODE) &&
        (interface->device[0] == '\0' || interface->speed == 0U)) {
        set_diagnostic(diagnostic, 0U, RNS_ERROR_PROTOCOL,
                       "enabled serial interface '%s' requires port and speed", interface->name);
        return RNS_ERROR_PROTOCOL;
    }
    return RNS_OK;
}

rns_status_t rns_config_parse(const char *text,
                              size_t text_length,
                              rns_config_t *config,
                              rns_config_diagnostic_t *diagnostic) {
    size_t position = 0U;
    size_t line_number = 0U;
    parse_section_t section = SECTION_NONE;
    rns_config_interface_t *current = NULL;

    if (text == NULL || config == NULL || (text_length != 0U && memchr(text, '\0', text_length) != NULL)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    rns_config_init(config);
    if (diagnostic != NULL) memset(diagnostic, 0, sizeof(*diagnostic));
    while (position < text_length) {
        char line[CONFIG_LINE_MAX];
        size_t start = position;
        size_t length;
        char *content;
        char *comment;
        char *separator;
        while (position < text_length && text[position] != '\n') ++position;
        length = position - start;
        if (position < text_length) ++position;
        ++line_number;
        if (length != 0U && text[start + length - 1U] == '\r') --length;
        if (length >= sizeof(line)) {
            set_diagnostic(diagnostic, line_number, RNS_ERROR_OVERFLOW, "configuration line is too long");
            return RNS_ERROR_OVERFLOW;
        }
        memcpy(line, text + start, length);
        line[length] = '\0';
        comment = strpbrk(line, "#;");
        if (comment != NULL) *comment = '\0';
        content = trim(line);
        if (*content == '\0') continue;
        if (content[0] == '[') {
            size_t content_length = strlen(content);
            if (content_length >= 4U && content[0] == '[' && content[1] == '[' &&
                content[content_length - 2U] == ']' && content[content_length - 1U] == ']') {
                char *name;
                content[content_length - 2U] = '\0';
                name = trim(content + 2U);
                if (section != SECTION_INTERFACES && section != SECTION_INTERFACE) {
                    set_diagnostic(diagnostic, line_number, RNS_ERROR_PROTOCOL,
                                   "interface subsection appears outside [interfaces]");
                    return RNS_ERROR_PROTOCOL;
                }
                if (*name == '\0' || config->interface_count == RNS_CONFIG_MAX_INTERFACES) {
                    set_diagnostic(diagnostic, line_number, RNS_ERROR_OVERFLOW,
                                   "empty interface name or too many interfaces");
                    return RNS_ERROR_OVERFLOW;
                }
                current = &config->interfaces[config->interface_count++];
                memset(current, 0, sizeof(*current));
                current->speed = 9600U;
                current->preamble_ms = 350U;
                current->tx_tail_ms = 20U;
                current->slot_time_ms = 20U;
                current->persistence = 64U;
                current->data_bits = 8U;
                current->parity = 'N';
                current->stop_bits = 1U;
                if (!copy_value(current->name, sizeof(current->name), name)) {
                    set_diagnostic(diagnostic, line_number, RNS_ERROR_OVERFLOW, "interface name is too long");
                    return RNS_ERROR_OVERFLOW;
                }
                section = SECTION_INTERFACE;
                continue;
            }
            if (content_length >= 2U && content[content_length - 1U] == ']') {
                content[content_length - 1U] = '\0';
                content = trim(content + 1U);
                current = NULL;
                if (strcmp(content, "reticulum") == 0) section = SECTION_RETICULUM;
                else if (strcmp(content, "interfaces") == 0) section = SECTION_INTERFACES;
                else {
                    set_diagnostic(diagnostic, line_number, RNS_ERROR_UNSUPPORTED,
                                   "unsupported configuration section '[%s]'", content);
                    return RNS_ERROR_UNSUPPORTED;
                }
                continue;
            }
            set_diagnostic(diagnostic, line_number, RNS_ERROR_PROTOCOL, "malformed section header");
            return RNS_ERROR_PROTOCOL;
        }
        separator = strchr(content, '=');
        if (separator == NULL) {
            set_diagnostic(diagnostic, line_number, RNS_ERROR_PROTOCOL, "expected key = value");
            return RNS_ERROR_PROTOCOL;
        }
        *separator = '\0';
        {
            char *key = trim(content);
            char *value = trim(separator + 1U);
            rns_status_t status;
            if (*key == '\0' || *value == '\0') {
                set_diagnostic(diagnostic, line_number, RNS_ERROR_PROTOCOL, "empty key or value");
                return RNS_ERROR_PROTOCOL;
            }
            if (section == SECTION_RETICULUM) {
                status = parse_reticulum_value(config, key, value, line_number, diagnostic);
            } else if (section == SECTION_INTERFACE && current != NULL) {
                status = parse_interface_value(current, key, value, line_number, diagnostic);
            } else {
                set_diagnostic(diagnostic, line_number, RNS_ERROR_PROTOCOL,
                               "option '%s' is not inside a supported value section", key);
                return RNS_ERROR_PROTOCOL;
            }
            if (status != RNS_OK) return status;
        }
    }
    for (position = 0U; position < config->interface_count; ++position) {
        rns_status_t status = validate_interface(&config->interfaces[position], diagnostic);
        if (status != RNS_OK) return status;
    }
    return RNS_OK;
}

typedef struct emitter {
    char *output;
    size_t capacity;
    size_t length;
} emitter_t;

static bool emit(emitter_t *emitter, const char *format, ...) {
    int written;
    va_list arguments;
    if (emitter->length >= emitter->capacity) return false;
    va_start(arguments, format);
    written = vsnprintf(emitter->output + emitter->length, emitter->capacity - emitter->length,
                        format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= emitter->capacity - emitter->length) return false;
    emitter->length += (size_t)written;
    return true;
}

rns_status_t rns_config_emit(const rns_config_t *config,
                             char *output,
                             size_t output_capacity,
                             size_t *output_length) {
    emitter_t emitter = {output, output_capacity, 0U};
    size_t index;
    if (config == NULL || output == NULL || output_length == NULL || output_capacity == 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *output_length = 0U;
    if (!emit(&emitter, "[reticulum]\n  enable_transport = %s\n  share_instance = %s\n"
                        "  shared_instance_type = %s\n  instance_name = %s\n"
                        "  panic_on_interface_error = %s\n  shared_instance_port = %u\n"
                        "  instance_control_port = %u\n\n[interfaces]\n",
              config->enable_transport ? "Yes" : "No", config->share_instance ? "Yes" : "No",
              config->shared_instance_type == RNS_CONFIG_SHARED_INSTANCE_UNIX ? "unix" : "tcp",
              config->instance_name,
              config->panic_on_interface_error ? "Yes" : "No",
              (unsigned int)config->shared_instance_port,
              (unsigned int)config->instance_control_port)) return RNS_ERROR_OVERFLOW;
    for (index = 0U; index < config->interface_count; ++index) {
        const rns_config_interface_t *item = &config->interfaces[index];
        const char *type_name = rns_config_interface_type_name(item->type);
        if (!item->type_set || type_name == NULL ||
            !emit(&emitter, "  [[%s]]\n    type = %s\n    enabled = %s\n",
                  item->name, type_name, item->enabled ? "Yes" : "No")) return RNS_ERROR_PROTOCOL;
        if (item->type == RNS_CONFIG_TCP_CLIENT &&
            !emit(&emitter, "    target_host = %s\n    target_port = %u\n", item->target_host,
                  (unsigned int)item->target_port)) return RNS_ERROR_OVERFLOW;
        if (item->type == RNS_CONFIG_TCP_SERVER &&
            !emit(&emitter, "    listen_ip = %s\n    listen_port = %u\n", item->listen_ip,
                  (unsigned int)item->listen_port)) return RNS_ERROR_OVERFLOW;
        if (item->type == RNS_CONFIG_UDP &&
            !emit(&emitter, "    listen_ip = %s\n    listen_port = %u\n    forward_ip = %s\n"
                           "    forward_port = %u\n", item->listen_ip,
                  (unsigned int)item->listen_port, item->forward_ip,
                  (unsigned int)item->forward_port)) return RNS_ERROR_OVERFLOW;
        if ((item->type == RNS_CONFIG_KISS || item->type == RNS_CONFIG_RNODE) &&
            !emit(&emitter, "    port = %s\n    speed = %u\n", item->device,
                  (unsigned int)item->speed)) return RNS_ERROR_OVERFLOW;
        if (item->type == RNS_CONFIG_KISS &&
            !emit(&emitter,
                  "    preamble = %u\n    txtail = %u\n    persistence = %u\n"
                  "    slottime = %u\n    flow_control = %s\n"
                  "    databits = %u\n    parity = %c\n    stopbits = %u\n",
                  (unsigned int)item->preamble_ms,
                  (unsigned int)item->tx_tail_ms,
                  (unsigned int)item->persistence,
                  (unsigned int)item->slot_time_ms,
                  item->flow_control ? "Yes" : "No",
                  (unsigned int)item->data_bits, item->parity,
                  (unsigned int)item->stop_bits)) return RNS_ERROR_OVERFLOW;
        if (item->type == RNS_CONFIG_RNODE &&
            !emit(&emitter, "    frequency = %u\n    bandwidth = %u\n    txpower = %d\n"
                           "    spreadingfactor = %u\n    codingrate = %u\n",
                  (unsigned int)item->frequency, (unsigned int)item->bandwidth,
                  (int)item->tx_power, (unsigned int)item->spreading_factor,
                  (unsigned int)item->coding_rate)) return RNS_ERROR_OVERFLOW;
        if (!emit(&emitter, "\n")) return RNS_ERROR_OVERFLOW;
    }
    *output_length = emitter.length;
    return RNS_OK;
}
