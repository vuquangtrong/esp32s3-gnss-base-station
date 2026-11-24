#ifndef NTRIP_CASTER_H
#define NTRIP_CASTER_H

#include "esp_err.h"

#define NTRIP_CASTER_PORT       2101
#define NTRIP_CASTER_MOUNTPOINT "BASE"
#define NTRIP_CASTER_USERNAME   "BASE"
#define NTRIP_CASTER_PASSWORD   "BASE"

#define NTRIP_CASTER_MAX_CLIENTS     4
#define NTRIP_CASTER_REQ_BUF_SIZE    512
#define NTRIP_CASTER_RESP_BUF_SIZE   512
#define NTRIP_CASTER_UART_BUF_SIZE   2048
#define NTRIP_CASTER_UART_TIMEOUT_MS 50
#define NTRIP_CASTER_RECV_TIMEOUT_MS 1000
#define NTRIP_CASTER_IDLE_DELAY_MS   100

esp_err_t ntrip_caster_init(void);
void ntrip_caster_start(void);
void ntrip_caster_stop(void);

#endif  // NTRIP_CASTER_H
