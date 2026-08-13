#include "gnss.h"

#include "esp_log.h"
#include "uart.h"

// UBlox UART1 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol with GGA, GLL, GSA, GSV, RMC, VTG, TXT messages are output
//      by default. UBX and RTCM 3.3 protocols are enabled by default but no
//      output messages are enabled by default
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      UBX, NMEA and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

// UBlox UART2 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA and UBX protocols are disabled by default.
//      RTCM 3.3 protocols is enabled by default but no output messages are
//      enabled by default
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol is disabled by default.
//      UBX and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

// UBlox settings for diffrent GNSS Modes:
//                  Rover                               Base                PPP
// UART1    TX      UBX:NAV-PVT:out + NMEA:GGA:out      UBX:NAV-PVT:out     UBX:NAV-PVT:out
//          RX      UBX:Command:in                      UBX:Command:in      UBX:Command:in
//
// UART2    TX      none                                RTCM3:out           UBX:RAW:out
//          RX      RTCM3:in                            none                none

static void gnss_set_default_messages(void)
{
    ESP_LOGI("GNSS", "Setting default GNSS messages");

    // UBlox UART1 TX
    // enable NMEA output to read GGA messages once every 10 epochs
    // some NMEA messages are enabled by default, so disable them
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART1 10");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 1");
    // disable RTCM3 output
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-RTCM3X 0");
    // enable UBX output to read NAV-PVT messages
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_NAV_PVT_UART1 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-UBX 1");

    // UBlox UART1 RX
    // disable NMEA input
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-NMEA 0");
    // disable RTCM3 input
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-RTCM3X 0");
    // !!! MUST ALWAYS ENABLE UBX input to send commands to UBlox
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-UBX 1");

    // UBlox UART2 TX
    // disable NMEA output
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 0");
    // disable RTCM3 output
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-RTCM3X 0");
    // disable UBX output
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-UBX 0");

    // UBlox UART2 RX
    // disable NMEA input
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-NMEA 0");
    // disable RTCM3 input
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-RTCM3X 0");
    // disable UBX input
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-UBX 0");
}

void gnss_set_mode_rover(void)
{
    gnss_set_default_messages();
    ESP_LOGI("GNSS", "Setting GNSS mode to ROVER");

    // In Rover mode,
    // ESP32 reads GGA and NAV-PVT messages from UBlox UART1
    // ESP32 sends RTCM3 correction data to UBlox UART2

    // UBlox UART1 TX
    // already configured in gnss_set_default_messages()

    // UBlox UART2 TX
    // enable RTCM3 input to receive correction data from ESP32
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-RTCM3X 1");

    status_set(STT_GNSS_MODE, GNSS_ROVER);
}

void gnss_set_mode_base(void)
{
    gnss_set_default_messages();
    ESP_LOGI("GNSS", "Setting GNSS mode to BASE");

    // In Base mode,
    // ESP32 reads PVT messages from UBlox UART1, RTCM3 messages from UBlox UART2
    // ESP32 does not send any correction data to UBlox UART2.

    // UBlox UART1 TX
    // disable NMEA output, keep UBX output to read NAV-PVT messages
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 0");

    // UBlox UART2 TX
    // enable RTCM3 output to send correction data to ESP32
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1005_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1074_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1077_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1084_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1087_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1094_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1097_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1124_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1127_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE1230_UART2 1");
    // uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE4072_0_UART2 1");
    // uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-RTCM_3X_TYPE4072_1_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-RTCM3X 1");

    status_set(STT_GNSS_MODE, GNSS_BASE);
}

void gnss_set_mode_ppp(void)
{
    gnss_set_default_messages();
    ESP_LOGI("GNSS", "Setting GNSS mode to PPP");

    // In PPP mode,
    // ESP32 reads PVT messages from UBlox UART1, RAW messages from UBlox UART2
    // ESP32 does not send any correction data to UBlox UART2.

    // UBlox UART1 TX
    // disable NMEA output, keep UBX output to read NAV-PVT messages
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 0");

    // UBlox UART2 TX
    // enable UBX output to read RAW messages
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_RXM_RAWX_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_RXM_SFRBX_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-UBX 1");

    status_set(STT_GNSS_MODE, GNSS_PPP);
}
