#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_observation_utils.h"
static void test_device_observation_mapping(void)
{
    assert(si_observation_used_percent(1000, 250) == 75.0);
    assert(si_observation_used_percent(1000, 1500) == 0.0);
    assert(si_observation_used_percent(0, 0) == 0.0);

    char uptime[32];
    si_observation_format_uptime(90060, uptime, sizeof(uptime));
    assert(strcmp(uptime, "1d 1h 1m") == 0);

    size_t start = 99;
    assert(si_observation_recent_window(3, 8, 8, 5, &start) == 5);
    assert(start == 6);
    assert(si_observation_recent_window(2, 2, 8, 5, &start) == 2);
    assert(start == 0);
    assert(si_observation_recent_window(0, 0, 8, 5, &start) == 0);
    assert(start == 0);
}

int main(void)
{
    test_device_observation_mapping();
    puts("stable KVM host core tests: PASS");
    return 0;
}
