#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>

#define STT_VALUE_LENGTH_MAX 128

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
} sdcard_status_t;

typedef enum
{
    LOGGER_STOPPED,
    LOGGER_RUNNING,
    LOGGER_ERROR
} logger_status_t;

typedef enum
{
    FW_UPDATE_IDLE,
    FW_UPDATE_VERIFYING,
    FW_UPDATE_UPDATING_APP,
    FW_UPDATE_UPDATING_WWW,
    FW_UPDATE_SUCCESS,
    FW_UPDATE_FAILED
} fw_update_status_t;

typedef enum
{
    // Battery
    STT_BAT_VOLT,

    // WiFi
    STT_WIFI_STATUS,
    STT_WIFI_IP_ADDR,

    // GNSS Mode
    STT_GNSS_MODE,

    // SDCard
    STT_SDCARD_STATUS,

    // Logger
    STT_LOGGER_STATUS,
    STT_LOGGER_FILE,
    STT_LOGGER_SIZE,

    // NTRIP Client
    STT_NTRIP_CLIENT_STATUS,
    STT_NTRIP_RECEIVED_BYTES,

    // NTRIP Server
    STT_NTRIP_SERVER_STATUS,
    STT_NTRIP_CONN_COUNT,
    STT_NTRIP_SENT_BYTES,

    // Firmware Update
    STT_FW_UPDATE_AVAILABLE,
    STT_FW_UPDATE_STATUS,

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
    int32_t i_value;
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

void status_set_int(status_type_t key, int32_t value);
void status_set_double(status_type_t key, double value);
void status_set_str(status_type_t key, const char* value);

int32_t status_get_int(status_type_t key);
double status_get_double(status_type_t key);
const char* status_get_str(status_type_t key);

const char* status_get_all(void);

#endif  // STATUS_H
