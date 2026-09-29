#include "time_utils.h"

#include "esp_timer.h"

uint32_t si_monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
