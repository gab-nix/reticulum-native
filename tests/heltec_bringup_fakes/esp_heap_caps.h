#ifndef TEST_ESP_HEAP_CAPS_H
#define TEST_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_8BIT 1U
size_t heap_caps_get_free_size(unsigned caps);
#endif
