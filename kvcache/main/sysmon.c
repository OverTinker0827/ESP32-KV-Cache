#include "sysmon.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sysmon_cpu(uint64_t *uptime_us, uint64_t *idle0_us, uint64_t *idle1_us)
{
    *uptime_us = esp_timer_get_time();
    *idle0_us = ulTaskGetIdleRunTimeCounterForCore(0);
    *idle1_us = ulTaskGetIdleRunTimeCounterForCore(1);
}

const char *variant_server_name(void)
{
#if CONFIG_KV_SERVER_BASIC
    return "basic-task-per-client";
#else
    return "opt-eventloop";
#endif
}

const char *variant_cores_name(void)
{
#if CONFIG_KV_SERVER_BASIC
    return "unpinned-tasks";
#elif CONFIG_KV_CORES_SINGLE
    return "single-core1";
#elif CONFIG_KV_CORES_SINGLE_CORE0
    return "single-core0";
#elif CONFIG_KV_CORES_SHARED
    return "dual-shared-lock";
#else
    return "dual-sharded";
#endif
}
