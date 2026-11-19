#include "crowpanel/adsb_client.h"

#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "crowpanel/chunked_decoder.h"
#include "crowpanel/http_head.h"
#include "crowpanel/snapshot_clock.h"

namespace services::adsb {

namespace {

constexpr char kHost[] = "opendata.adsb.fi";
constexpr uint16_t kPort = 443;
constexpr char kPathBase[] = "/api/v3/lat/";
constexpr int kHttpOk = 200;
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;
// A trailer that never arrives must not hold a finished response hostage.
constexpr unsigned long kTrailerGraceMs = 100;

// Keeping the connection open between fetches means only the first one pays
// for a TLS handshake (over a second on these chips, with the UI blocked).
// It also holds the TLS buffers for good, so only the PSRAM board does it.
#ifdef PLANE_RADAR_CROWPANEL
constexpr bool kKeepConnection = true;
#else
constexpr bool kKeepConnection = false;
#endif

WiFiClientSecure* s_client = nullptr;
// Not on the stack: the UI is polled, and redraws, while a request waits.
char s_head_text[1536];

#ifdef PLANE_RADAR_CROWPANEL
// 256 aircraft are 34 KB. On the board with PSRAM they go there, leaving
// internal RAM for TLS.
Aircraft* s_aircraft = nullptr;

bool haveAircraftStorage() {
  if (s_aircraft == nullptr) {
    s_aircraft = static_cast<Aircraft*>(heap_caps_calloc_prefer(
        kMaxAircraft, sizeof(Aircraft), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_DEFAULT));
  }
  return s_aircraft != nullptr;
}
#else
Aircraft s_aircraft[kMaxAircraft];

bool haveAircraftStorage() { return true; }
#endif
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
unsigned long s_last_success_ms = 0;
unsigned long s_last_attempt_ms = 0;
SnapshotClock s_snapshot_clock;
unsigned long s_snapshot_ms = 0;
int s_last_http_status = 0;
char s_last_error[64] = "not fetched";

void setLastError(const char* message) {
  strncpy(s_last_error, message, sizeof(s_last_error) - 1);
  s_last_error[sizeof(s_last_error) - 1] = '\0';
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

// Time of day in milliseconds since the epoch, or 0 until it has been set
// from the network (an unset clock counts from 1970).
uint64_t wallClockMs() {
  constexpr time_t kSetAfter = 1700000000;
  struct timeval now = {};
  gettimeofday(&now, nullptr);
  if (now.tv_sec < kSetAfter) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The request is written and its reply read here rather than through
// HTTPClient, whose GET() blocks until the response head arrives. That wait is
// the server's whole processing time, several hundred milliseconds with the
// touch screen frozen, once per fetch.

// Connects if the kept connection is gone. The TCP connect is tried in short
// slices so the UI is polled; the TLS handshake after it still blocks.
bool ensureConnected(WiFiClientSecure& client) {
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (!client.connected()) {
    if (millis() >= deadline) {
      return false;
    }
    pollNetwork();
    if (!client.connect(kHost, kPort, kConnectAttemptMs)) {
      delay(5);
    }
  }
  return true;
}

bool sendRequest(WiFiClientSecure& client, const String& path) {
  String request = "GET ";
  request += path;
  request += " HTTP/1.1\r\nHost: ";
  request += kHost;
  request += "\r\nUser-Agent: ESP32-Plane-Radar/1.0\r\nAccept: application/json"
             "\r\nConnection: keep-alive\r\n\r\n";
  return client.print(request) == request.length();
}

bool readHeadWithPoll(WiFiClientSecure& client, HttpHead* head) {
  size_t length = 0;
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    while (client.available() > 0) {
      const int c = client.read();
      if (c < 0) {
        break;
      }
      if (length + 1 >= sizeof(s_head_text)) {
        return false;
      }
      s_head_text[length++] = static_cast<char>(c);
      if (length >= 4 && memcmp(s_head_text + length - 4, "\r\n\r\n", 4) == 0) {
        s_head_text[length] = '\0';
        return parseHttpHead(s_head_text, head);
      }
    }
    if (!client.connected()) {
      return false;
    }
    delay(1);
  }
  return false;
}

// The largest reply worth reading. A wide-radius reply naming every aircraft
// is a few hundred kilobytes; a server sending far more is not the feed, and
// is not allowed to fill the memory.
constexpr size_t kMostBodyBytes = 1024 * 1024;

enum class Body { Complete, Empty, TooLarge };

Body readBodyWithPoll(WiFiClientSecure& client, const HttpHead& head,
                      String& payload) {
  if (head.content_length > static_cast<int>(kMostBodyBytes)) {
    return Body::TooLarge;
  }
  if (head.content_length > 0) {
    payload.reserve(static_cast<unsigned>(head.content_length + 1));
  }

  uint8_t buffer[512];
  ChunkedDecoder decoder;
  unsigned long body_complete_ms = 0;
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int available = client.available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = client.read(buffer, static_cast<size_t>(to_read));
      if (read_bytes > 0) {
        const size_t body_bytes =
            head.chunked ? decoder.decode(buffer, static_cast<size_t>(read_bytes))
                         : static_cast<size_t>(read_bytes);
        if (payload.length() + body_bytes > kMostBodyBytes) {
          return Body::TooLarge;
        }
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(body_bytes));
      }
    }
    if (head.chunked && decoder.done()) {
      break;
    }
    if (head.chunked && decoder.bodyComplete()) {
      if (body_complete_ms == 0) {
        body_complete_ms = millis();
      } else if (millis() - body_complete_ms > kTrailerGraceMs) {
        break;
      }
    }
    if (head.content_length > 0 &&
        static_cast<int>(payload.length()) >= head.content_length) {
      break;
    }
    if (client.available() <= 0 && !client.connected()) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0 ? Body::Complete : Body::Empty;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

// A number such as 1e300 is valid JSON and becomes infinity as a float. No
// such value is ever passed on: the arithmetic downstream assumes real numbers.
bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    const float value = obj[key].as<float>();
    if (std::isfinite(value)) {
      *out = value;
      return true;
    }
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (plane["ground"].is<bool>() && plane["ground"].as<bool>()) {
    return true;
  }
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

bool readAltitude(const JsonObject& plane, int32_t* altitude_ft) {
  float value = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &value) ||
      readJsonFloat(plane, "alt_geom", &value)) {
    *altitude_ft = static_cast<int32_t>(lroundf(value));
    return true;
  }
  *altitude_ft = 0;
  return false;
}

int32_t readVerticalRate(const JsonObject& plane) {
  float value = 0.0f;
  if (readJsonFloat(plane, "baro_rate", &value) ||
      readJsonFloat(plane, "geom_rate", &value)) {
    return static_cast<int32_t>(lroundf(value));
  }
  return 0;
}

float readSeenSeconds(const JsonObject& plane) {
  float value = 0.0f;
  return readJsonFloat(plane, "seen", &value) ? value : 0.0f;
}

float readPositionAge(const JsonObject& plane) {
  float value = 0.0f;
  if (readJsonFloat(plane, "seen_pos", &value)) {
    return value;
  }
  return readSeenSeconds(plane);
}

bool hasMotionVector(const JsonObject& plane) {
  float value = 0.0f;
  return readJsonFloat(plane, "gs", &value) &&
         (readJsonFloat(plane, "track", &value) ||
          readJsonFloat(plane, "true_heading", &value) ||
          readJsonFloat(plane, "mag_heading", &value));
}

bool hasEmergency(const JsonObject& plane, const char* squawk) {
  if (strcmp(squawk, "7500") == 0 || strcmp(squawk, "7600") == 0 ||
      strcmp(squawk, "7700") == 0) {
    return true;
  }
  if (plane["emergency"].is<const char*>()) {
    const char* value = plane["emergency"].as<const char*>();
    return value[0] != '\0' && strcmp(value, "none") != 0;
  }
  return false;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    strncpy(ac->callsign, ac->hex, sizeof(ac->callsign) - 1);
    ac->callsign[sizeof(ac->callsign) - 1] = '\0';
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  copyJsonStringTrimmed(plane, "r", ac->registration, sizeof(ac->registration));
  copyJsonStringTrimmed(plane, "category", ac->category, sizeof(ac->category));
  copyJsonStringTrimmed(plane, "squawk", ac->squawk, sizeof(ac->squawk));
  copyJsonStringTrimmed(plane, "desc", ac->description, sizeof(ac->description));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  ac->on_ground = isOnGround(plane);
  ac->altitude_valid = readAltitude(plane, &ac->alt_ft);
  ac->vertical_rate_fpm = readVerticalRate(plane);
  ac->seen_sec = readSeenSeconds(plane);
  ac->emergency = hasEmergency(plane, ac->squawk);
  // Bit 0 of dbFlags marks military airframes.
  ac->military = ((plane["dbFlags"] | 0) & 1) != 0;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void closeConnection() {
  if (s_client != nullptr) {
    s_client->stop();
  }
}

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastSuccessMs() { return s_last_success_ms; }

unsigned long snapshotMs() { return s_snapshot_ms; }

unsigned long lastAttemptMs() { return s_last_attempt_ms; }

int lastHttpStatus() { return s_last_http_status; }

const char* lastError() { return s_last_error; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  s_last_attempt_ms = millis();
  s_last_http_status = 0;
  if (!haveAircraftStorage()) {
    setLastError("out of memory");
    return false;
  }
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String path = kPathBase;
  path += String(center_lat, 6);
  path += "/lon/";
  path += String(center_lon, 6);
  path += "/dist/";
  path += String(dist_nm, 1);

  if (s_client == nullptr) {
    s_client = new WiFiClientSecure();
    s_client->setInsecure();
  }
  WiFiClientSecure& client = *s_client;

  // The server may have closed a kept connection since the last fetch, so a
  // request that fails on one is tried once more on a fresh connection.
  HttpHead head;
  bool answered = false;
  for (int attempt = 0; attempt < 2 && !answered; ++attempt) {
    const bool reused = client.connected();
    answered = ensureConnected(client) && sendRequest(client, path) &&
               readHeadWithPoll(client, &head);
    if (!answered) {
      client.stop();
      if (!reused) {
        break;
      }
    }
  }
  if (!answered) {
    Serial.println("adsb: no response");
    setLastError("no response");
    return false;
  }
  const unsigned long arrived_ms = millis();
  const int64_t arrived_steady_ms = esp_timer_get_time() / 1000;
  const uint64_t arrived_wall_ms = wallClockMs();
  s_last_http_status = head.status;
  if (head.status != kHttpOk) {
    Serial.printf("adsb: HTTP %d\n", head.status);
    snprintf(s_last_error, sizeof(s_last_error), "HTTP %d", head.status);
    closeConnection();
    return false;
  }

  String payload;
  const Body body = readBodyWithPoll(client, head, payload);
  if (body != Body::Complete) {
    const char* problem = body == Body::Empty ? "empty response" : "response too large";
    Serial.printf("adsb: %s\n", problem);
    setLastError(problem);
    closeConnection();
    return false;
  }
  if (!kKeepConnection || !head.keep_alive) {
    closeConnection();
  }

  // Keep only fields used by the firmware. Wide-radius responses can otherwise
  // consume several megabytes by retaining every readsb/tar1090 field.
  // to<JsonObject>() creates filter["ac"][0]. Reading it as a JsonVariant
  // returns an unbound reference, which leaves the filter empty and makes
  // every response parse to nothing.
  JsonDocument filter;
  filter["now"] = true;
  JsonObject aircraft_filter = filter["ac"][0].to<JsonObject>();
  const char* retained_fields[] = {
      "lat",         "lon",       "hex",       "flight",   "t",
      "r",           "desc",      "category",  "squawk",   "emergency",
      "alt_baro",    "alt_geom",  "ground",    "true_heading",
      "mag_heading", "track",     "dir",       "gs",       "tas",
      "ias",         "baro_rate", "geom_rate", "seen",     "seen_pos",
      "dbFlags",
  };
  for (const char* field : retained_fields) {
    aircraft_filter[field] = true;
  }

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    snprintf(s_last_error, sizeof(s_last_error), "JSON: %s", err.c_str());
    return false;
  }

  // "now" is when the feed took this snapshot: milliseconds since the epoch
  // here, seconds in other readsb-style feeds.
  double taken = doc["now"] | 0.0;
  if (taken > 0.0 && taken < 1e11) {
    taken *= 1000.0;
  }
  const uint32_t snapshot_age_ms = s_snapshot_clock.ageOnArrival(
      static_cast<uint64_t>(taken), arrived_steady_ms, arrived_wall_ms);
  s_snapshot_ms = arrived_ms - snapshot_age_ms;

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    s_last_success_ms = millis();
    setLastError("none");
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    // A position off the globe would turn into distances that are not
    // numbers, and sorting those reads outside the list.
    float lat = 0.0f;
    float lon = 0.0f;
    if (!readJsonFloat(plane, "lat", &lat) || !readJsonFloat(plane, "lon", &lon) ||
        lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft
#ifdef PLANE_RADAR_CROWPANEL
        && false
#endif
    ) {
      continue;
    }

    s_aircraft[n].lat = lat;
    s_aircraft[n].lon = lon;
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    s_aircraft[n].position_age_sec = readPositionAge(plane);
    s_aircraft[n].motion_valid = hasMotionVector(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  s_last_success_ms = millis();
  setLastError("none");
  Serial.printf("adsb: %u aircraft, snapshot %u ms old\n",
                static_cast<unsigned>(n), static_cast<unsigned>(snapshot_age_ms));
  return true;
}

}  // namespace services::adsb
