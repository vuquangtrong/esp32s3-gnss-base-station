#include "gnss.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "ntrip_caster.h"
#include "ntrip_client.h"
#include "status.h"
#include "uart.h"

#define GNSS_CMD_BUF_SIZE 128

static const char* TAG = "gnss";

// UBlox UART1 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol with GGA, GLL, GSA, GSV, RMC, VTG, TXT messages are output by default.
//      UBX and RTCM 3.3 protocols are enabled by default but no output messages are enabled by default.
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      UBX, NMEA and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

// UBlox UART2 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA and UBX protocols are disabled by default.
//      RTCM 3.3 protocol is enabled by default but no output messages are enabled by default.
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol is disabled by default.
//      UBX and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

// UBlox settings for different GNSS Modes:
//          Ublox   Rover               Base                PPP
// UART1    TX      UBX:NAV-PVT:out     UBX:NAV-PVT:out     UBX:NAV-PVT:out
//          RX      UBX:Command:in      UBX:Command:in      UBX:Command:in
//
// UART2    TX      NMEA:GGA:out        RTCM3:out           UBX:RAW:out
//          RX      RTCM3:in            none                none

static esp_err_t gnss_send_commands(const char* const* cmds, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        ESP_RETURN_ON_ERROR(uart1_send_command(cmds[i]), TAG, "Failed command: %s", cmds[i]);
    }
    return ESP_OK;
}

static esp_err_t gnss_set_default_messages(void)
{
    ESP_LOGI(TAG, "Setting default GNSS messages");

    static const char* const cmds[] = {
        // Disable Time Mode (Survey-In / Fixed Base) first so reconfiguring
        // Base coordinates or restarting Survey-In always starts from a clean state
        "CFG-VALSET 0 1 0 0 CFG-TMODE-MODE 0",

        // UBlox UART2 TX: disable all output protocols immediately to stop active NMEA / RTCM3 / UBX streams
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 0",
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-RTCM3X 0",
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-UBX 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART2 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART2 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART2 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART2 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART2 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART2 0",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART2 0", // not implemented NMEA_ID_TXT in ublox.c

        // UBlox UART2 RX: disable all input protocols
        "CFG-VALSET 0 1 0 0 CFG-UART2INPROT-NMEA 0",
        "CFG-VALSET 0 1 0 0 CFG-UART2INPROT-RTCM3X 0",
        "CFG-VALSET 0 1 0 0 CFG-UART2INPROT-UBX 0",

        // UBlox UART1 TX:
        // Silence NMEA & RTCM3 protocols first, then disable all default NMEA messages
        // Keep UBX NAV-PVT enabled.
        "CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 0",
        "CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-RTCM3X 0",
        "CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-UBX 1",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART1 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART1 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART1 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART1 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART1 0",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART1 0",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART1 0", // not implemented NMEA_ID_TXT in ublox.c
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_NAV_PVT_UART1 2",  // once every 2 epochs

        // UBlox UART1 RX:
        // Disable NMEA and RTCM3 input
        // !!! MUST ALWAYS ENABLE UBX input to send commands to UBlox
        "CFG-VALSET 0 1 0 0 CFG-UART1INPROT-NMEA 0",
        "CFG-VALSET 0 1 0 0 CFG-UART1INPROT-RTCM3X 0",
        "CFG-VALSET 0 1 0 0 CFG-UART1INPROT-UBX 1",
    };

    return gnss_send_commands(cmds, sizeof(cmds) / sizeof(cmds[0]));
}

esp_err_t gnss_set_mode_rover(void)
{
    ntrip_caster_stop();
    ntrip_client_disconnect();
    ESP_RETURN_ON_ERROR(gnss_set_default_messages(), TAG, "Failed to set default messages");
    ESP_LOGI(TAG, "Setting GNSS mode to ROVER");

    // In Rover mode:
    // ESP32 reads NAV-PVT messages from UBlox UART1 and reads GGA from UBlox UART2
    // ESP32 sends RTCM3 correction data to UBlox UART2
    static const char* const cmds[] = {
        // UBlox UART1 TX: enable NAV-PVT output once every 2 epochs
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_NAV_PVT_UART1 2",
        "CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-UBX 1",

        // UBlox UART2 TX: enable NMEA GGA output once every 10 epochs
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART2 10",
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 1",

        // UBlox UART2 RX: enable RTCM3 input to receive correction data from ESP32
        "CFG-VALSET 0 1 0 0 CFG-UART2INPROT-RTCM3X 1",
    };

    ESP_RETURN_ON_ERROR(gnss_send_commands(cmds, sizeof(cmds) / sizeof(cmds[0])), TAG, "Failed to configure ROVER mode");
    ESP_RETURN_ON_ERROR(ntrip_client_start_gga_task(), TAG, "Failed to start NTRIP GGA task");

    status_set_int(STT_GNSS_MODE, GNSS_ROVER);
    return ESP_OK;
}

esp_err_t gnss_set_mode_base(
    double latitude,   // deg, 9 decimal places
    double longitude,  // deg, 9 decimal places
    double height      // m, 4 decimal places
)
{
    esp_err_t ret = ESP_OK;

    if (!isfinite(latitude) || latitude < -90.0 || latitude > 90.0 || !isfinite(longitude) || longitude < -180.0 || longitude > 180.0 || !isfinite(height) ||
        fabs(height) > 100000.0)
    {
        ESP_LOGE(TAG, "Invalid base position: lat=%f, lon=%f, height=%f", latitude, longitude, height);
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_caster_stop();
    ntrip_client_disconnect();
    ntrip_client_stop_gga_task();

    ESP_GOTO_ON_ERROR(gnss_set_default_messages(), fail, TAG, "Failed to set default messages");
    ESP_LOGI(TAG, "Setting GNSS mode to BASE: lat=%.9f, lon=%.9f, height=%.4f", latitude, longitude, height);

    // Split LLH into coarse + high-precision components for u-blox CFG-TMODE
    int64_t latitude_e9 = llround(latitude * 1e9);
    int32_t lat_pos = (int32_t)(latitude_e9 / 100);  // scale: 1e-7, in degrees
    int32_t lat_hp = (int32_t)(latitude_e9 % 100);   // scale: 1e-9, in degrees

    int64_t longitude_e9 = llround(longitude * 1e9);
    int32_t lon_pos = (int32_t)(longitude_e9 / 100);  // scale: 1e-7, in degrees
    int32_t lon_hp = (int32_t)(longitude_e9 % 100);   // scale: 1e-9, in degrees

    int64_t height_e4 = llround(height * 1e4);
    int32_t height_pos = (int32_t)(height_e4 / 100);  // in centimeters
    int32_t height_hp = (int32_t)(height_e4 % 100);   // scale: 0.1, in millimeters

    static char cmd[GNSS_CMD_BUF_SIZE];

    // Set LLH position type and fixed position parameters before enabling Fixed Base mode
    ESP_GOTO_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-TMODE-POS_TYPE 1"), fail, TAG, "Failed to set POS_TYPE");

    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-LAT %" PRIi32, lat_pos);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set LAT");
    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-LAT_HP %" PRIi32, lat_hp);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set LAT_HP");

    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-LON %" PRIi32, lon_pos);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set LON");
    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-LON_HP %" PRIi32, lon_hp);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set LON_HP");

    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-HEIGHT %" PRIi32, height_pos);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set HEIGHT");
    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-HEIGHT_HP %" PRIi32, height_hp);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set HEIGHT_HP");

    // Set fixed position accuracy to 5 cm (500 x 0.1 mm = 50 mm = 5 cm)
    ESP_GOTO_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-TMODE-FIXED_POS_ACC 500"), fail, TAG, "Failed to set FIXED_POS_ACC");

    // Enable Fixed Base mode
    ESP_GOTO_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-TMODE-MODE 2"), fail, TAG, "Failed to enable BASE mode");

    // In Base mode:
    // ESP32 reads NAV-PVT messages from UBlox UART1, RTCM3 messages from UBlox UART2
    // ESP32 does not send any correction data to UBlox UART2
    static const char* const rtcm_cmds[] = {
        // UBlox UART2 TX: enable RTCM3 MSM4 messages to send correction data to ESP32
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1005_UART2 1",
        // MSM4 message
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1074_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1084_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1094_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1124_UART2 1",
        // MSM7 message are disabled for better compatibility with old receivers
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1077_UART2 1",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1087_UART2 1",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1097_UART2 1",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1127_UART2 1",
        // RTCM3 Type 1230 message is used for GLONASS L1/L2 code-phase corrections
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1230_UART2 1",
        // RTCM3 Type 4072 message is used for u-blox moving baseline reference station PVT
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE4072_0_UART2 1",
        // "CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE4072_1_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-RTCM3X 1",
    };
    ESP_GOTO_ON_ERROR(gnss_send_commands(rtcm_cmds, sizeof(rtcm_cmds) / sizeof(rtcm_cmds[0])), fail, TAG, "Failed to configure RTCM3");

    ntrip_caster_start();
    status_set_int(STT_GNSS_MODE, GNSS_BASE);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "Falling back to ROVER mode");
    return gnss_set_mode_rover();
}

esp_err_t gnss_set_mode_ppp(
    uint32_t duration,  // seconds
    uint32_t accuracy   // mm
)
{
    esp_err_t ret = ESP_OK;

    if (duration == 0 || accuracy == 0 || accuracy > (UINT32_MAX / 10))
    {
        ESP_LOGE(TAG, "Invalid PPP parameters: duration=%" PRIu32 " s, accuracy=%" PRIu32 " mm", duration, accuracy);
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_caster_stop();
    ntrip_client_disconnect();
    ntrip_client_stop_gga_task();

    ESP_GOTO_ON_ERROR(gnss_set_default_messages(), fail, TAG, "Failed to set default messages");
    ESP_LOGI(TAG, "Setting GNSS mode to PPP: duration=%" PRIu32 " s, accuracy=%" PRIu32 " mm", duration, accuracy);

    static char cmd[GNSS_CMD_BUF_SIZE];

    // Set survey-in duration (seconds) and accuracy limit (scale: 0.1 mm) before enabling Survey-In mode
    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-SVIN_MIN_DUR %" PRIu32, duration);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set SVIN_MIN_DUR");

    snprintf(cmd, sizeof(cmd), "CFG-VALSET 0 1 0 0 CFG-TMODE-SVIN_ACC_LIMIT %" PRIu32, accuracy * 10);
    ESP_GOTO_ON_ERROR(uart1_send_command(cmd), fail, TAG, "Failed to set SVIN_ACC_LIMIT");

    // Enable Survey-In Base Mode
    ESP_GOTO_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-TMODE-MODE 1"), fail, TAG, "Failed to enable PPP mode");

    // In PPP mode:
    // ESP32 reads NAV-PVT messages from UBlox UART1, RAW messages from UBlox UART2
    // ESP32 does not send any correction data to UBlox UART2
    static const char* const raw_cmds[] = {
        // UBlox UART2 TX: enable UBX output to read RAWX and SFRBX messages
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_RXM_RAWX_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_RXM_SFRBX_UART2 1",
        "CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-UBX 1",
    };
    ESP_GOTO_ON_ERROR(gnss_send_commands(raw_cmds, sizeof(raw_cmds) / sizeof(raw_cmds[0])), fail, TAG, "Failed to configure RAW messages");

    status_set_int(STT_GNSS_MODE, GNSS_PPP);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "Falling back to ROVER mode");
    return gnss_set_mode_rover();
}
