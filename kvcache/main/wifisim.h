#pragma once
/* Synthetic stand-in for the load WiFi + lwIP put on the chip, used while the board's
 * supply cannot power the radio. NOT a model validated against real WiFi yet:
 * re-calibrate the knobs against measured WiFi CPU/ISR load once the radio works.
 *
 * Per 1 ms period, on `core`, at high priority (like the WiFi task):
 *   - copies `bufs` packet-sized (1600 B) heap buffers   -> heap allocator + SRAM bus traffic
 *   - reads `flash_kb` KiB of flash-resident data       -> evicts that core's flash cache,
 *                                                          as WiFi/lwIP code in flash does
 *   - spins until `duty_pct` % of the period is used     -> CPU stolen from the cache
 * plus a hardware timer interrupt at `isr_hz`            -> packet-interrupt preemption */
typedef struct {
    int core;
    int duty_pct;
    int bufs;
    int flash_kb;
    int isr_hz;
} wifisim_cfg_t;

/* flash_kb = 2: a cache-line miss costs ~4 us (32 B over 40 MHz DIO flash), so 2 KiB/ms
 * = 64 misses = ~256 us/ms, inside the 40 % budget. 16 KiB/ms overran it (measured). */
#define WIFISIM_DEFAULT(c) {.core = (c), .duty_pct = 40, .bufs = 2, .flash_kb = 2, .isr_hz = 2000}

void wifisim_start(const wifisim_cfg_t *cfg);
void wifisim_stop(void);
/* measured share of each period the simulator actually consumed (sanity check) */
int wifisim_measured_duty_pct(void);
