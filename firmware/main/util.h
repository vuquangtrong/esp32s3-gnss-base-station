#ifndef ESP32S3_GNSS_UTIL_H
#define ESP32S3_GNSS_UTIL_H

#include <esp_log.h>

// log ERROR with more info
#define ERROR(format, ...) ESP_LOGE(TAG, "%s:%d (%s): " format, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)

// log ERROR on Condition
#define ERROR_IF(condition, action, format, ...)                                               \
    if ((condition))                                                                           \
    {                                                                                          \
        ESP_LOGE(TAG, "%s:%d (%s): " format, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
        action;                                                                                \
    }

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define NEWLINE "\n"
#define CARRET  "\r"

#endif  // ESP32S3_GNSS_UTIL_H
