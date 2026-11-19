#include "services/route_lookup.h"

#include "services/route_fit.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace services::route {
namespace {

constexpr char kApiBase[] = "https://api.adsbdb.com/v0/callsign/";
constexpr unsigned long kTimeoutMs = 6000;

// Answers are kept so that an aircraft seen again is not looked up again. A
// flight number flies the same route all day, so a found route is good for
// hours; "no such route" is retried sooner in case the source learns it, and a
// failed request sooner still.
constexpr size_t kCacheSize = 24;
constexpr uint32_t kFoundLifetimeMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kNotFoundLifetimeMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kFailedLifetimeMs = 60UL * 1000UL;
// ADSBDB is a free public service: never more than one request in this long.
constexpr uint32_t kPauseBetweenLookupsMs = 1500;
constexpr uint32_t kTaskStackBytes = 8192;
// A route is about 2 KB of JSON. Nothing longer is read.
constexpr size_t kMostBodyBytes = 8192;
// How long pause() waits for a lookup to finish: a little over its timeout.
constexpr uint32_t kLongestWaitMs = 8000;

struct Entry {
  bool used;
  Info info;
  uint32_t stored_ms;
  uint32_t lifetime_ms;
};

// Shared between the caller's task and the lookup task; s_lock guards all of
// it. Nothing is held across the network request.
Entry s_cache[kCacheSize] = {};
char s_wanted[9] = {};    // Asked for and not yet picked up by the task.
Place s_wanted_at = {};   // Where that aircraft is.
char s_fetching[9] = {};  // Being fetched now.
SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t s_wake = nullptr;
volatile bool s_updated = false;
bool s_paused = false;  // Guarded by s_lock.

// Takes a response body up to a fixed size and refuses the rest, so that a
// server cannot make the device hold whatever it cares to send.
class BoundedBody : public Stream {
 public:
  BoundedBody(char* text, size_t size) : text_(text), size_(size) { text_[0] = '\0'; }

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    if (length >= size_ - length_) {
      return 0;  // HTTPClient gives up when a write comes back short.
    }
    memcpy(text_ + length_, data, length);
    length_ += length;
    text_[length_] = '\0';
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  char* text_;
  size_t size_;
  size_t length_ = 0;
};

void sanitizeCallsign(const char* source, char* destination, size_t length) {
  size_t output = 0;
  if (source != nullptr) {
    for (size_t i = 0; source[i] != '\0' && output + 1 < length; ++i) {
      const unsigned char value = static_cast<unsigned char>(source[i]);
      if (isalnum(value)) {
        destination[output++] = static_cast<char>(toupper(value));
      }
    }
  }
  destination[output] = '\0';
}

void copyText(JsonVariantConst value, char* destination, size_t length) {
  destination[0] = '\0';
  if (!value.is<const char*>()) {
    return;
  }
  strncpy(destination, value.as<const char*>(), length - 1);
  destination[length - 1] = '\0';
}

void copyAirport(JsonObjectConst airport, char* destination, size_t length) {
  copyText(airport["iata_code"], destination, length);
  if (destination[0] == '\0') {
    copyText(airport["icao_code"], destination, length);
  }
}

// False if the source gave no coordinates for the airport.
bool readPlace(JsonObjectConst airport, Place* place) {
  if (!airport["latitude"].is<double>() || !airport["longitude"].is<double>()) {
    return false;
  }
  place->lat = airport["latitude"].as<double>();
  place->lon = airport["longitude"].as<double>();
  return true;
}

// Whether the aircraft is on any leg of the route. A leg whose airports have
// no coordinates cannot be checked and is taken on trust.
bool routeFits(JsonObjectConst route, const Place& aircraft) {
  const JsonObjectConst stops[] = {route["origin"].as<JsonObjectConst>(),
                                   route["midpoint"].as<JsonObjectConst>(),
                                   route["destination"].as<JsonObjectConst>()};
  Place places[3];
  size_t count = 0;
  for (const JsonObjectConst& stop : stops) {
    if (stop.isNull()) {
      continue;  // Most routes have no midpoint.
    }
    if (!readPlace(stop, &places[count])) {
      return true;
    }
    ++count;
  }
  for (size_t leg = 0; leg + 1 < count; ++leg) {
    if (onLeg(aircraft, places[leg], places[leg + 1])) {
      return true;
    }
  }
  return count < 2;
}

// The entry for a callsign if there is one that has not expired. Call with
// s_lock held.
Entry* cached(const char* callsign) {
  const uint32_t now = millis();
  for (Entry& entry : s_cache) {
    if (entry.used && strcmp(entry.info.callsign, callsign) == 0) {
      if (now - entry.stored_ms < entry.lifetime_ms) {
        return &entry;
      }
      entry.used = false;
    }
  }
  return nullptr;
}

// Call with s_lock held.
void store(const Info& info, uint32_t lifetime_ms) {
  Entry* slot = &s_cache[0];
  const uint32_t now = millis();
  for (Entry& entry : s_cache) {
    if (!entry.used) {
      slot = &entry;
      break;
    }
    if (now - entry.stored_ms > now - slot->stored_ms) {
      slot = &entry;  // The oldest so far.
    }
  }
  slot->used = true;
  slot->info = info;
  slot->stored_ms = now;
  slot->lifetime_ms = lifetime_ms;
}

// One request to ADSBDB. Fills `info` and returns how long the answer should
// be kept.
uint32_t fetch(const char* callsign, const Place& aircraft, Info* info) {
  *info = Info{};
  strncpy(info->callsign, callsign, sizeof(info->callsign) - 1);
  if (WiFi.status() != WL_CONNECTED) {
    return kFailedLifetimeMs;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = kApiBase;
  url += callsign;
  if (!http.begin(client, url)) {
    return kFailedLifetimeMs;
  }
  http.setTimeout(kTimeoutMs);
  http.addHeader("User-Agent", "ESP32-Plane-Radar/1.0");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    if (code == HTTP_CODE_NOT_FOUND) {
      return kNotFoundLifetimeMs;
    }
    Serial.printf("route: %s HTTP %d\n", callsign, code);
    return kFailedLifetimeMs;
  }
  const std::unique_ptr<char[]> payload(new (std::nothrow) char[kMostBodyBytes]);
  if (!payload) {
    http.end();
    return kFailedLifetimeMs;
  }
  BoundedBody body(payload.get(), kMostBodyBytes);
  const int written = http.writeToStream(&body);
  http.end();
  if (written < 0) {
    Serial.printf("route: %s reply not read (%d)\n", callsign, written);
    return kFailedLifetimeMs;
  }

  JsonDocument document;
  if (deserializeJson(document, payload.get())) {
    return kFailedLifetimeMs;
  }
  const JsonObjectConst route = document["response"]["flightroute"];
  if (route.isNull()) {
    return kNotFoundLifetimeMs;
  }
  copyAirport(route["origin"].as<JsonObjectConst>(), info->origin,
              sizeof(info->origin));
  copyAirport(route["midpoint"].as<JsonObjectConst>(), info->midpoint,
              sizeof(info->midpoint));
  copyAirport(route["destination"].as<JsonObjectConst>(), info->destination,
              sizeof(info->destination));
  copyText(route["airline"]["name"], info->airline, sizeof(info->airline));
  info->route_found = info->origin[0] != '\0' && info->destination[0] != '\0';
  if (!info->route_found) {
    return kNotFoundLifetimeMs;
  }
  if (!routeFits(route, aircraft)) {
    Serial.printf("route: %s is not on %s>%s, ignored\n", info->callsign,
                  info->origin, info->destination);
    *info = Info{};
    strncpy(info->callsign, callsign, sizeof(info->callsign) - 1);
    return kNotFoundLifetimeMs;
  }
  Serial.printf("route: %s %s>%s%s%s\n", info->callsign, info->origin,
                info->midpoint[0] ? info->midpoint : "",
                info->midpoint[0] ? ">" : "", info->destination);
  return kFoundLifetimeMs;
}

void lookupTask(void*) {
  for (;;) {
    xSemaphoreTake(s_wake, portMAX_DELAY);
    char callsign[sizeof(s_wanted)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(callsign, s_wanted);
    const Place aircraft = s_wanted_at;
    s_wanted[0] = '\0';
    const bool needed =
        !s_paused && callsign[0] != '\0' && cached(callsign) == nullptr;
    if (needed) {
      strcpy(s_fetching, callsign);
    }
    xSemaphoreGive(s_lock);
    if (!needed) {
      continue;
    }

    Info info;
    const uint32_t lifetime_ms = fetch(callsign, aircraft, &info);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    store(info, lifetime_ms);
    s_fetching[0] = '\0';
    xSemaphoreGive(s_lock);
    s_updated = true;
    vTaskDelay(pdMS_TO_TICKS(kPauseBetweenLookupsMs));
  }
}

}  // namespace

void begin() {
  if (s_lock != nullptr) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  s_wake = xSemaphoreCreateBinary();
  if (s_lock == nullptr || s_wake == nullptr) {
    return;
  }
  // On the Wi-Fi core: the TLS handshake is a second of arithmetic, and the
  // other core is the one that draws.
  xTaskCreatePinnedToCore(lookupTask, "routes", kTaskStackBytes, nullptr, 1,
                          nullptr, 0);
}

void request(const char* callsign, double lat, double lon) {
  char wanted[sizeof(s_wanted)];
  sanitizeCallsign(callsign, wanted, sizeof(wanted));
  if (wanted[0] == '\0' || s_lock == nullptr) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool needed = !s_paused && cached(wanted) == nullptr &&
                      strcmp(s_fetching, wanted) != 0 && strcmp(s_wanted, wanted) != 0;
  if (needed) {
    // Only the latest request waits: one for an aircraft that has since left
    // the card is not worth making.
    strcpy(s_wanted, wanted);
    s_wanted_at = {lat, lon};
  }
  xSemaphoreGive(s_lock);
  if (needed) {
    xSemaphoreGive(s_wake);
  }
}

bool find(const char* callsign, Info* info) {
  char wanted[sizeof(s_wanted)];
  sanitizeCallsign(callsign, wanted, sizeof(wanted));
  if (wanted[0] == '\0' || s_lock == nullptr) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const Entry* entry = cached(wanted);
  if (entry != nullptr) {
    *info = entry->info;
  }
  xSemaphoreGive(s_lock);
  return entry != nullptr;
}

bool updated() {
  const bool was = s_updated;
  s_updated = false;
  return was;
}

void pause(bool paused) {
  if (s_lock == nullptr) {
    return;
  }
  // The task decides whether to start a lookup with the lock held, so once
  // this has seen none under way with the pause set, none can start.
  const uint32_t started_ms = millis();
  for (;;) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_paused = paused;
    if (paused) {
      s_wanted[0] = '\0';
    }
    const bool under_way = s_fetching[0] != '\0';
    xSemaphoreGive(s_lock);
    if (!paused || !under_way || millis() - started_ms > kLongestWaitMs) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace services::route
