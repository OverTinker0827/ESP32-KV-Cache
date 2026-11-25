#include <stdlib.h>
#include <string.h>
#include "wifisim.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* 128 KiB of read-only data that lives in flash (DROM): reading it goes through the
 * same per-core flash cache that instruction fetches from flash use. */
#define FLASH_TBL (128 * 1024)
static const uint8_t flash_tbl[FLASH_TBL] = {[0] = 1, [FLASH_TBL - 1] = 2};

static volatile int running;
static volatile uint32_t sink;
static volatile uint32_t isr_count;
static volatile int64_t busy_us, total_us;
static TaskHandle_t task;
static SemaphoreHandle_t done;
static gptimer_handle_t tmr;
static wifisim_cfg_t cfg;

static bool IRAM_ATTR on_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    isr_count++; /* a short ISR, like a packet-RX interrupt handing off to a task */
    return false;
}

/* Period = 2 ticks (2 ms) so the busy part (duty% of 2 ms) always ends before the
 * deadline and the task really sleeps. Rates are per ms, so per period they are x2. */
#define PERIOD_TICKS 2
#define PERIOD_US 2000

static void sim_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    int64_t start_us = esp_timer_get_time();
    uint32_t foff = 0;
    while (running) {
        int64_t t0 = esp_timer_get_time();
        /* packet buffers: alloc, fill, read back, free */
        for (int i = 0; i < cfg.bufs * PERIOD_TICKS; i++) {
            uint8_t *b = malloc(1600);
            if (b) {
                memset(b, (uint8_t)i, 1600);
                sink += b[i * 7 % 1600];
                free(b);
            }
        }
        /* flash-cache pressure: touch one byte per 32 B cache line */
        const volatile uint8_t *ft = flash_tbl; /* volatile: every read must reach the cache */
        for (int k = 0; k < cfg.flash_kb * PERIOD_TICKS * 1024; k += 32) {
            sink += ft[foff];
            foff = (foff + 32) % FLASH_TBL;
        }
        /* burn the remaining share of the period */
        int64_t until = t0 + (int64_t)cfg.duty_pct * PERIOD_US / 100;
        while (esp_timer_get_time() < until) sink++;
        int64_t used = esp_timer_get_time() - t0;
        busy_us += used;
        total_us = esp_timer_get_time() - start_us; /* wall time, so overruns show up honestly */
        /* never monopolise the core: if the work overran the period, still sleep one tick */
        if (used >= PERIOD_US) {
            vTaskDelay(1);
            last = xTaskGetTickCount();
        } else {
            vTaskDelayUntil(&last, PERIOD_TICKS);
        }
    }
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

void wifisim_start(const wifisim_cfg_t *c)
{
    cfg = *c;
    busy_us = total_us = 0;
    running = 1;
    if (!done) done = xSemaphoreCreateBinary();
    if (cfg.isr_hz > 0) {
        gptimer_config_t tc = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                               .resolution_hz = 1000000};
        gptimer_new_timer(&tc, &tmr);
        gptimer_event_callbacks_t cb = {.on_alarm = on_alarm};
        gptimer_register_event_callbacks(tmr, &cb, NULL);
        gptimer_alarm_config_t ac = {.alarm_count = 1000000 / cfg.isr_hz, .reload_count = 0,
                                     .flags.auto_reload_on_alarm = true};
        gptimer_set_alarm_action(tmr, &ac);
        gptimer_enable(tmr);
        gptimer_start(tmr);
    }
    /* priority 22: above the cache's tasks, just below where the real WiFi task runs (23) */
    xTaskCreatePinnedToCore(sim_task, "wifisim", 3072, NULL, 22, &task, cfg.core);
}

void wifisim_stop(void)
{
    running = 0;
    xSemaphoreTake(done, portMAX_DELAY);
    if (tmr) {
        gptimer_stop(tmr);
        gptimer_disable(tmr);
        gptimer_del_timer(tmr);
        tmr = NULL;
    }
}

int wifisim_measured_duty_pct(void) { return total_us ? (int)(busy_us * 100 / total_us) : 0; }
