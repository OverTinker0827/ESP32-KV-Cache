// SPI link test, ESP8266 side (hardware SPI slave, HSPI pins: MOSI GPIO13/D7, MISO GPIO12/D6,
// SCK GPIO14/D5, CS GPIO15/D8). The ESP32 master writes a 32-byte frame
// [seq u32][24 payload bytes][crc32 u32]; this side checks the CRC, prepares a reply
// [seq][payload ^ 0xA5][crc32] in the read buffer and only then publishes seq in the status
// register (top bit set if the CRC was wrong), so the master knows the reply is ready.
#include <SPISlave.h>
#include <ESP8266WiFi.h>
#include "wifi_creds.h"

#ifndef RADIO_ON
#define RADIO_ON 1  // 1: join WiFi (the real bridge condition); 0: radio off
#endif

static uint32_t crc32b(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
  }
  return ~c;
}

static volatile uint32_t frames = 0, bad = 0;

void setup() {
  Serial.begin(115200);
  WiFi.persistent(false);
#if RADIO_ON
  WiFi.mode(WIFI_STA);
  {
    int n = WiFi.scanNetworks();
    int found = 0, ch = 0, rssi = 0;
    int near = 0;
    String want = String(WIFI_SSID);
    want.trim();
    want.toLowerCase();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == WIFI_SSID) found = 1, ch = WiFi.channel(i), rssi = WiFi.RSSI(i);
      String got = WiFi.SSID(i);
      got.trim();
      got.toLowerCase();
      if (got == want || got.indexOf(want) >= 0 || (got.length() >= 4 && want.indexOf(got) >= 0)) near++;
    }
    Serial.printf("\nWIFI SCAN near_matches(case/space/substring)=%d\n", near);
    Serial.printf("\nWIFI SCAN networks=%d target_found=%d channel=%d rssi=%d ssid_len=%d pass_len=%d\n", n, found, ch,
                  rssi, (int)strlen(WIFI_SSID), (int)strlen(WIFI_PASS));
  }
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#else
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
#endif
  SPISlave.onData([](uint8_t *d, size_t len) {
    uint32_t seq, crc;
    memcpy(&seq, d, 4);
    memcpy(&crc, d + 28, 4);
    bool ok = len >= 32 && crc32b(d, 28) == crc;
    uint8_t r[32];
    memcpy(r, d, 4);
    for (int i = 4; i < 28; i++) r[i] = d[i] ^ 0xA5;
    uint32_t rc = crc32b(r, 28);
    memcpy(r + 28, &rc, 4);
    SPISlave.setData(r, 32);
    frames++;
    if (!ok) bad++;
    SPISlave.setStatus((seq & 0x7FFFFFFF) | (ok ? 0 : 0x80000000));
  });
  SPISlave.onDataSent([]() {});
  SPISlave.onStatus([](uint32_t) {});
  SPISlave.onStatusSent([]() {});
  SPISlave.begin(4);
  SPISlave.setStatus(0x5A5A5A5A);  // "ready" marker the master looks for
  Serial.println("\nSPISLAVE READY");
}

void loop() {
  static uint32_t t = 0, last = 0;
  static bool joined = false;
  if (!joined && WiFi.status() == WL_CONNECTED) {
    joined = true;
    Serial.printf("WIFI CONNECTED rssi=%d dBm\n", WiFi.RSSI());
  }
  if (millis() - t > 2000) {
    t = millis();
#if RADIO_ON
    if (!joined) Serial.printf("WIFI status=%d\n", (int)WiFi.status()); /* 1 no SSID, 4 failed, 6 wrong password, 7 idle/connecting */
#endif
    uint32_t f = frames;
    if (f != last) Serial.printf("SLAVE frames=%u crc_bad=%u\n", f, (unsigned)bad);
    last = f;
  }
}
