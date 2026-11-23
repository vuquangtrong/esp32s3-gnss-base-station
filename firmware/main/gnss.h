#ifndef GNSS_H
#define GNSS_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t gnss_set_mode_rover(void);
esp_err_t gnss_set_mode_base(double latitude, double longitude, double height);
esp_err_t gnss_set_mode_ppp(uint32_t duration, uint32_t accuracy);

#endif  // GNSS_H
