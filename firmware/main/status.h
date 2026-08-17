#ifndef STATUS_H
#define STATUS_H

#define STT_VALUE_LENGTH_MAX (64)

typedef enum
{
    GNSS_ROVER,
    GNSS_BASE,
    GNSS_PPP
} gnss_mode_t;

typedef enum
{
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_CONNECTED
} connection_status_t;

typedef enum
{
    SDCARD_REMOVED,
    SDCARD_MOUNTED,
    SDCARD_ERROR
} sdcard_state_t;

typedef enum
{
    LOGGER_STOPPED,
    LOGGER_RUNNING,
    LOGGER_ERROR
} logger_state_t;

typedef enum
{
    // Battery
    STT_BAT_VOLT,
    // WiFi
    STT_WIFI_STATUS,
    STT_WIFI_IP_ADDR,
    // GNSS Mode
    STT_GNSS_MODE,
    // GNSS Position
    STT_GNSS_TIME,
    STT_GNSS_LAT,
    STT_GNSS_LON,
    STT_GNSS_ALT,
    STT_GNSS_SAT,
    STT_GNSS_HACC,
    STT_GNSS_VACC,
    STT_GNSS_FIX,
    // SDCard
    STT_SDCARD_STATUS,
    // Logger
    STT_LOGGER_STATUS,
    STT_LOGGER_FILE,
    STT_LOGGER_SIZE,
    // NTRIP Client
    STT_NTRIP_CLIENT_STATUS,
    STT_NTRIP_RECEIVED_BYTES,
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

void status_set_int(status_type_t key, int value);
void status_set_double(status_type_t key, double value);
void status_set_str(status_type_t key, const char* value);

int status_get_int(status_type_t key);
double status_get_double(status_type_t key);
const char* status_get_str(status_type_t key);

const char* status_get_all(void);

// Macro to automatically select the appropriate status_set function based on the type of the value passed in
#define status_set(key, val)                                                                                                                                             \
    _Generic((val), int: status_set_int, long: status_set_int, double: status_set_double, float: status_set_double, char*: status_set_str, const char*: status_set_str)( \
        key, val                                                                                                                                                         \
    )

#endif  // STATUS_H
