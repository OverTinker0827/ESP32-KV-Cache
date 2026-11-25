/* SPI link test, ESP32 side (master on VSPI: MOSI 23, MISO 19, SCK 18, CS 5 driven by hand).
 * Speaks the ESP8266 hardware-slave protocol: 0x02 write 32 B, 0x03 read 32 B, 0x04 read status.
 * CS idles LOW (ESP8266 GPIO15 must be low when it boots) and is pulsed high around every
 * transaction, as in the Arduino "SafeMaster" example.
 * Sweep: handshake (status polling / read until the reply carries our sequence number)
 *      x idle gap after each transaction x clock. Each cell: NFRAMES frames of
 *      write -> wait for the reply -> read, every byte verified.
 * Prints RESULT {json} lines and SPITEST DONE. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_MOSI 23
#define PIN_MISO 19
#define PIN_SCK 18
#define PIN_CS 5
#define NFRAMES 1000
#define PULSE_US 2

static spi_device_handle_t dev;
static int gap_us = 20; /* idle time after every transaction: the slave's ISR resets its SPI unit */

static uint32_t crc32b(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
    }
    return ~c;
}

static inline void pulse(void)
{
    gpio_set_level(PIN_CS, 1);
    esp_rom_delay_us(PULSE_US);
    gpio_set_level(PIN_CS, 0);
}

static int cs_idle_high = 1; /* 1: standard SPI (CS low only during a transaction); 0: idle low + pulses */
static void xfer(const uint8_t *tx, uint8_t *rx, int n)
{
    spi_transaction_t t = {.length = n * 8, .tx_buffer = tx, .rx_buffer = rx};
    if (cs_idle_high) {
        gpio_set_level(PIN_CS, 0);
        esp_rom_delay_us(1);
        spi_device_polling_transmit(dev, &t);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_CS, 1);
    } else {
        pulse();
        spi_device_polling_transmit(dev, &t);
        pulse();
    }
    esp_rom_delay_us(gap_us);
}

static uint32_t read_status(void)
{
    uint8_t tx[5] = {0x04}, rx[5];
    xfer(tx, rx, 5);
    return rx[1] | rx[2] << 8 | rx[3] << 16 | (uint32_t)rx[4] << 24;
}

static void write_data(const uint8_t *d)
{
    uint8_t tx[34] = {0x02, 0x00}, rx[34];
    memcpy(tx + 2, d, 32);
    xfer(tx, rx, 34);
}

static void read_data(uint8_t *d)
{
    uint8_t tx[34] = {0x03, 0x00}, rx[34];
    xfer(tx, rx, 34);
    memcpy(d, rx + 2, 32);
}

static int spi_mode = 1;
static void set_clock(int hz)
{
    if (dev) spi_bus_remove_device(dev);
    spi_device_interface_config_t dc = {
        .mode = spi_mode, .clock_speed_hz = hz, .spics_io_num = -1, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dc, &dev));
}

static uint32_t xs = 0x12345678;
static uint32_t rnd(void)
{
    xs ^= xs << 13;
    xs ^= xs >> 17;
    xs ^= xs << 5;
    return xs;
}

void app_main(void)
{
    gpio_reset_pin(PIN_CS);
    gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CS, 0);
    spi_bus_config_t bc = {.mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
                           .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bc, SPI_DMA_DISABLED));
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 1. is the slave there? (tests MISO and the command path) */
    set_clock(1000000);
    uint32_t st = read_status();
    printf("LINK %s (status 0x%08" PRIx32 ")\n", st == 0x5A5A5A5A ? "slave ready marker seen" : "no ready marker", st);

    /* 3. reliability with a link layer: CRC on every frame + retry (re-send on a bad write,
     *    re-read on a bad reply). 20,000 frames per configuration; a delivered frame must match exactly. */
    struct { int mode, hz; gpio_drive_cap_t cap; } cfgs[] = {
        {1, 10000000, GPIO_DRIVE_CAP_1}, {1, 16000000, GPIO_DRIVE_CAP_1}, {1, 10000000, GPIO_DRIVE_CAP_0}};
    uint32_t seq = 1;
    gap_us = 20;
    cs_idle_high = 0;
    for (int k = 0; k < 3; k++) {
        spi_mode = cfgs[k].mode;
        set_clock(cfgs[k].hz);
        gpio_set_drive_capability(PIN_SCK, cfgs[k].cap);
        gpio_set_drive_capability(PIN_MOSI, cfgs[k].cap);
        gpio_set_drive_capability(PIN_CS, cfgs[k].cap);
        const int N = 20000, MAXTRY = 8;
        uint32_t delivered = 0, wrong_delivered = 0, failed = 0, rewrites = 0, rereads = 0, waits = 0;
        uint32_t hist[9] = {0};
        int64_t t0 = esp_timer_get_time();
        for (int n = 0; n < N; n++, seq++) {
            uint8_t f[32], r[32];
            memcpy(f, &seq, 4);
            for (int i = 4; i < 28; i++) f[i] = (uint8_t)rnd();
            uint32_t c = crc32b(f, 28);
            memcpy(f + 28, &c, 4);
            int tries = 0, done_ok = 0;
            while (tries < MAXTRY && !done_ok) {
                tries++;
                write_data(f);
                int64_t tw = esp_timer_get_time();
                uint32_t s2 = 0;
                int got = 0;
                while (esp_timer_get_time() - tw < 5000) {
                    s2 = read_status();
                    waits++;
                    if ((s2 & 0x7FFFFFFF) == (seq & 0x7FFFFFFF)) { got = 1; break; }
                }
                if (!got || (s2 & 0x80000000)) { rewrites++; continue; } /* lost or corrupted on the way in */
                for (int rr = 0; rr < 4 && !done_ok; rr++) {
                    if (rr) rereads++;
                    read_data(r);
                    uint32_t rs, rc;
                    memcpy(&rs, r, 4);
                    memcpy(&rc, r + 28, 4);
                    if (rs != seq || rc != crc32b(r, 28)) continue; /* corrupted on the way back: read again */
                    done_ok = 1;
                    for (int i = 4; i < 28; i++)
                        if (r[i] != (f[i] ^ 0xA5)) { wrong_delivered++; break; }
                }
            }
            if (done_ok) { delivered++; hist[tries]++; }
            else failed++;
        }
        int64_t dt = esp_timer_get_time() - t0;
        printf("RESULT {\"part\":\"spilink_reliable\",\"spi_mode\":%d,\"clock_hz\":%d,\"drive\":%d,\"frames\":%d,"
               "\"delivered\":%" PRIu32 ",\"wrong_payload_delivered\":%" PRIu32 ",\"failed_after_%d_tries\":%" PRIu32
               ",\"rewrites\":%" PRIu32 ",\"rereads\":%" PRIu32 ",\"first_try\":%" PRIu32 ",\"second_try\":%" PRIu32
               ",\"third_or_later\":%" PRIu32 ",\"frames_per_s\":%.0f,\"us_per_frame\":%.1f,\"payload_kB_per_s_each_way\":%.1f}\n",
               cfgs[k].mode, cfgs[k].hz, (int)cfgs[k].cap, N, delivered, wrong_delivered, MAXTRY, failed, rewrites,
               rereads, hist[1], hist[2], delivered - hist[1] - hist[2], N * 1e6 / dt, (double)dt / N,
               N * 24.0 / dt * 1e6 / 1024);
    }
    gpio_set_level(PIN_CS, 0);
    printf("SPITEST DONE\n");
}
