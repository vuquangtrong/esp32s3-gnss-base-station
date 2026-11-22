#ifndef FACTORY_TEST_H
#define FACTORY_TEST_H

#include "esp_err.h"

#if FACTORY_TEST
esp_err_t factory_test_uart(void);
#endif

#endif  // FACTORY_TEST_H
