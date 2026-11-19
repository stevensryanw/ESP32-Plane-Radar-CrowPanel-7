#include "crowpanel/logger.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "crowpanel/settings.h"
#include "crowpanel/adsb_client.h"

namespace crowpanel::logger {
namespace {

constexpr int kSdSclk = 12;
constexpr int kSdMiso = 13;
constexpr int kSdMosi = 11;
constexpr int kSdCs = 10;
constexpr char kLogPath[] = "/plane-radar.csv";

SPIClass s_sd_spi(FSPI);
bool s_available = false;
char s_status[64] = "not initialized";

void setStatus(const char* status) {
  strncpy(s_status, status, sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = '\0';
}

void formatTimestamp(char* output, size_t length) {
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm utc = {};
    gmtime_r(&now, &utc);
    strftime(output, length, "%Y-%m-%dT%H:%M:%SZ", &utc);
  } else {
    snprintf(output, length, "uptime-%lu", millis() / 1000UL);
  }
}

}  // namespace

bool init() {
  s_sd_spi.begin(kSdSclk, kSdMiso, kSdMosi, kSdCs);
  if (!SD.begin(kSdCs, s_sd_spi, 16000000U)) {
    s_available = false;
    setStatus("no microSD card");
    Serial.println("CrowPanel: microSD not mounted");
    return false;
  }
  s_available = true;
  setStatus("microSD ready");
  Serial.printf("CrowPanel: microSD %llu MB\n",
                static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool available() { return s_available; }

const char* status() { return s_status; }

void logSnapshot() {
  if (!s_available || !settings::get().logging_enabled) {
    return;
  }

  const bool needs_header = !SD.exists(kLogPath);
  File file = SD.open(kLogPath, FILE_APPEND);
  if (!file) {
    setStatus("log open failed");
    return;
  }
  if (needs_header) {
    file.println(
        "timestamp,hex,callsign,type,registration,lat,lon,alt_ft,ground_speed_kt,"
        "track_deg,vertical_rate_fpm,squawk,category");
  }

  char timestamp[32];
  formatTimestamp(timestamp, sizeof(timestamp));
  const services::adsb::Aircraft* aircraft = services::adsb::aircraftList();
  const size_t count = services::adsb::aircraftCount();
  for (size_t i = 0; i < count; ++i) {
    const services::adsb::Aircraft& item = aircraft[i];
    file.printf("%s,%s,%s,%s,%s,%.6f,%.6f,", timestamp, item.hex,
                item.callsign, item.type, item.registration, item.lat, item.lon);
    if (item.altitude_valid) {
      file.print(item.alt_ft);
    }
    file.printf(",%.1f,%.1f,%ld,%s,%s\n", item.gs_knots, item.track_deg,
                static_cast<long>(item.vertical_rate_fpm), item.squawk,
                item.category);
  }
  file.close();
  setStatus("logging");
}

}  // namespace crowpanel::logger
