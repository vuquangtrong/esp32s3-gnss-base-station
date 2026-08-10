#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>

#define STT_VALUE_LENGTH_MAX (64)

#define status_set(key, val)                                                                                                                                             \
    _Generic((val), int: status_set_int, long: status_set_int, double: status_set_double, float: status_set_double, char*: status_set_str, const char*: status_set_str)( \
        key, val                                                                                                                                                         \
    )

typedef enum
{
    WIFI_DISCONNECT,
    WIFI_CONNECTING,
    WIFI_CONNECTED
} wifi_status_t;

typedef enum
{
    GNSS_ROVER,
    GNSS_BASE,
    GNSS_PPP
} gnss_mode_t;

typedef enum
{
    // WiFi Station
    STT_STA_STATUS,
    STT_STA_IP,
    // GNSS Mode
    STT_GNSS_MODE,
    // GNSS Position
    STT_GNSS_DATE,
    STT_GNSS_TIME,
    STT_GNSS_LAT,
    STT_GNSS_LON,
    STT_GNSS_ALT,
    STT_GNSS_SAT,
    STT_GNSS_FIX,
    STT_GNSS_HACC,
    STT_GNSS_VACC,
    //
    STT_MAX
} status_type_t;

typedef enum
{
    STT_VALUE_INT,
    STT_VALUE_DOUBLE,
    STT_VALUE_STRING
} status_value_type_t;

typedef union
{
    int i_value;
    double d_value;
    char str_value[STT_VALUE_LENGTH_MAX];
} status_value_u;

typedef struct
{
    const char* name;
    status_value_type_t type;
    status_value_u value;
} status_entry_t;

const char* status_name(status_type_t key);
status_value_type_t status_type(status_type_t key);
const char* status_get_all(void);

void status_set_int(status_type_t key, int value);
int status_get_int(status_type_t key);
void status_set_double(status_type_t key, double value);
double status_get_double(status_type_t key);
void status_set_str(status_type_t key, const char* value);
const char* status_get_str(status_type_t key);

#endif  // STATUS_H
