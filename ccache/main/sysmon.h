#pragma once
#include <stdint.h>
/* microseconds: uptime, and time each core spent in its idle task */
void sysmon_cpu(uint64_t *uptime_us, uint64_t *idle0_us, uint64_t *idle1_us);
const char *variant_server_name(void);
const char *variant_cores_name(void);
