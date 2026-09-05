#ifndef TEST_ESP_LOG_H
#define TEST_ESP_LOG_H
void test_bringup_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) test_bringup_log(__VA_ARGS__)
#define ESP_LOGE(...) test_bringup_log(__VA_ARGS__)
#endif
