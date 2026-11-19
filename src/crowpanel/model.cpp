#include "crowpanel/model.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crowpanel/geo.h"
#include "crowpanel/settings.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"

namespace crowpanel::model {
namespace {

struct HistorySlot {
  bool used = false;
  char hex[7] = {};
  HistoryPoint points[kHistoryPointCount] = {};
  uint8_t start = 0;
  uint8_t count = 0;
  uint32_t last_seen_ms = 0;
};

// A report older than this is not projected any further; the target may
// have turned, and a wrong guess is worse than a stale position.
constexpr float kMaxProjectionSeconds = 20.0f;
// One trail point every few seconds keeps a trail a couple of minutes long
// however often the feed is polled.
constexpr uint32_t kHistorySpacingMs = 5000;

// These two tables are about 65 KB together. They live in PSRAM, which has
// megabytes to spare, so that internal RAM is left for the network stack: two
// TLS sessions (the aircraft feed and route lookups) need most of it.
AircraftView* s_visible = nullptr;
size_t s_visible_count = 0;
uint32_t s_snapshot_ms = 0;
HistorySlot* s_history = nullptr;
char s_selected_hex[7] = {};
Alert s_alert = {};

// Zeroed storage is the tables' empty state. False if there is no memory, in
// which case there is simply no traffic to show.
bool haveStorage() {
  constexpr uint32_t kPreferPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (s_visible == nullptr) {
    s_visible = static_cast<AircraftView*>(heap_caps_calloc_prefer(
        services::adsb::kMaxAircraft, sizeof(AircraftView), 2, kPreferPsram,
        MALLOC_CAP_DEFAULT));
  }
  if (s_history == nullptr) {
    s_history = static_cast<HistorySlot*>(heap_caps_calloc_prefer(
        kHistoryAircraftCount, sizeof(HistorySlot), 2, kPreferPsram,
        MALLOC_CAP_DEFAULT));
  }
  return s_visible != nullptr && s_history != nullptr;
}

HistorySlot* findHistory(const char* hex, bool create) {
  if (hex == nullptr || hex[0] == '\0' || !haveStorage()) {
    return nullptr;
  }
  HistorySlot* oldest = nullptr;
  for (size_t index = 0; index < kHistoryAircraftCount; ++index) {
    HistorySlot& slot = s_history[index];
    if (slot.used && strcmp(slot.hex, hex) == 0) {
      return &slot;
    }
    if (!slot.used) {
      oldest = &slot;
      break;
    }
    if (oldest == nullptr || slot.last_seen_ms < oldest->last_seen_ms) {
      oldest = &slot;
    }
  }
  if (!create || oldest == nullptr) {
    return nullptr;
  }
  *oldest = HistorySlot{};
  oldest->used = true;
  strncpy(oldest->hex, hex, sizeof(oldest->hex) - 1);
  return oldest;
}

// Trails are made of reported positions, never projected ones.
void addHistoryPoint(const AircraftView& view) {
  HistorySlot* slot = findHistory(view.aircraft.hex, true);
  if (slot == nullptr) {
    return;
  }
  slot->last_seen_ms = millis();

  if (slot->count > 0) {
    const size_t last_index =
        (slot->start + slot->count - 1) % kHistoryPointCount;
    const HistoryPoint& previous = slot->points[last_index];
    if (millis() - previous.timestamp_ms < kHistorySpacingMs) {
      return;
    }
    const geo::Offset movement =
        geo::offsetFrom(previous.lat, previous.lon, view.fix_lat, view.fix_lon);
    // Suppress duplicate points while still preserving the slot's liveness.
    if (movement.distance_km < 0.02f &&
        millis() - previous.timestamp_ms < 30000UL) {
      return;
    }
  }

  size_t index = 0;
  if (slot->count < kHistoryPointCount) {
    index = (slot->start + slot->count) % kHistoryPointCount;
    ++slot->count;
  } else {
    index = slot->start;
    slot->start = (slot->start + 1) % kHistoryPointCount;
  }
  slot->points[index] = {view.fix_lat, view.fix_lon, millis()};
}

void updateAlert() {
  s_alert = Alert{};
  if (!settings::get().alerts_enabled) {
    return;
  }

  const AircraftView* nearby = nullptr;
  for (size_t i = 0; i < s_visible_count; ++i) {
    const AircraftView& view = s_visible[i];
    if (view.aircraft.emergency) {
      s_alert.active = true;
      s_alert.emergency = true;
      snprintf(s_alert.text, sizeof(s_alert.text), "EMERGENCY %s  SQ %s",
               view.aircraft.callsign,
               view.aircraft.squawk[0] ? view.aircraft.squawk : "----");
      return;
    }
    if (!view.aircraft.on_ground &&
        view.distance_km <= settings::get().proximity_km && nearby == nullptr) {
      nearby = &view;
    }
  }
  if (nearby != nullptr) {
    s_alert.active = true;
    s_alert.emergency = false;
    if (::ui::radar::useMiles()) {
      snprintf(s_alert.text, sizeof(s_alert.text), "NEAR HOME  %s  %.1f mi",
               nearby->aircraft.callsign, nearby->distance_km / 1.609344f);
    } else {
      snprintf(s_alert.text, sizeof(s_alert.text), "NEAR HOME  %s  %.1f km",
               nearby->aircraft.callsign, nearby->distance_km);
    }
  }
}

}  // namespace

void reset() {
  s_visible_count = 0;
  if (haveStorage()) {
    memset(s_history, 0, sizeof(HistorySlot) * kHistoryAircraftCount);
  }
  s_selected_hex[0] = '\0';
  s_alert = Alert{};
}

void update() {
  s_visible_count = 0;
  if (!haveStorage()) {
    return;
  }
  const services::adsb::Aircraft* aircraft = services::adsb::aircraftList();
  const size_t count = services::adsb::aircraftCount();
  const double center_lat = services::location::lat();
  const double center_lon = services::location::lon();

  for (size_t i = 0; i < count; ++i) {
    const services::adsb::Aircraft& item = aircraft[i];
    if (!settings::aircraftAltitudeVisible(item.on_ground, item.altitude_valid,
                                           item.alt_ft)) {
      continue;
    }
    const geo::Offset offset =
        geo::offsetFrom(center_lat, center_lon, item.lat, item.lon);
    AircraftView& view = s_visible[s_visible_count++];
    view.aircraft = item;
    view.fix_lat = item.lat;
    view.fix_lon = item.lon;
    view.distance_km = offset.distance_km;
    view.bearing_deg = offset.bearing_deg;
  }
  s_snapshot_ms = services::adsb::snapshotMs();
  advance(millis());

  std::sort(s_visible, s_visible + s_visible_count,
            [](const AircraftView& lhs, const AircraftView& rhs) {
              return lhs.distance_km < rhs.distance_km;
            });

  // Trails are intentionally limited to nearby traffic. Keeping 24 positions
  // for all 256 wide-area targets would consume too much internal RAM.
  const size_t history_count = std::min(s_visible_count, kHistoryAircraftCount);
  for (size_t i = 0; i < history_count; ++i) {
    addHistoryPoint(s_visible[i]);
  }
  if (s_selected_hex[0] != '\0') {
    const AircraftView* selected = selectedAircraft();
    if (selected != nullptr && selected->distance_km >
                                   s_visible[history_count > 0 ? history_count - 1 : 0]
                                       .distance_km) {
      addHistoryPoint(*selected);
    }
  }

  // Drop a selection only when it has genuinely disappeared from the new snapshot.
  if (s_selected_hex[0] != '\0' && selectedAircraft() == nullptr) {
    clearSelection();
  }
  updateAlert();
}

void advance(uint32_t now_ms) {
  const float since_snapshot = (now_ms - s_snapshot_ms) / 1000.0f;
  const double center_lat = services::location::lat();
  const double center_lon = services::location::lon();
  for (size_t i = 0; i < s_visible_count; ++i) {
    AircraftView& view = s_visible[i];
    if (!view.aircraft.motion_valid) {
      continue;
    }
    const float age = std::min(
        kMaxProjectionSeconds, view.aircraft.position_age_sec + since_snapshot);
    geo::advance(view.fix_lat, view.fix_lon, view.aircraft.track_deg,
                 view.aircraft.gs_knots, age, &view.aircraft.lat,
                 &view.aircraft.lon);
    const geo::Offset offset = geo::offsetFrom(center_lat, center_lon,
                                               view.aircraft.lat,
                                               view.aircraft.lon);
    view.distance_km = offset.distance_km;
    view.bearing_deg = offset.bearing_deg;
  }
}

const AircraftView* visibleAircraft() { return s_visible; }

size_t visibleAircraftCount() { return s_visible_count; }

void selectAircraft(const char* hex) {
  if (hex == nullptr) {
    clearSelection();
    return;
  }
  strncpy(s_selected_hex, hex, sizeof(s_selected_hex) - 1);
  s_selected_hex[sizeof(s_selected_hex) - 1] = '\0';
}

void clearSelection() { s_selected_hex[0] = '\0'; }

const char* selectedHex() { return s_selected_hex; }

const AircraftView* selectedAircraft() {
  if (s_selected_hex[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < s_visible_count; ++i) {
    if (strcmp(s_visible[i].aircraft.hex, s_selected_hex) == 0) {
      return &s_visible[i];
    }
  }
  return nullptr;
}

// Parked and taxiing aircraft do not always report that they are on the
// ground. Nothing with wings flies this slowly, so speed settles it; things
// that can hover (rotorcraft, and the lighter-than-air and gliding classes)
// are taken at their word.
static bool isFlying(const services::adsb::Aircraft& aircraft) {
  constexpr float kSlowestFlightKnots = 40.0f;
  if (aircraft.on_ground) {
    return false;
  }
  const bool can_hover =
      strcmp(aircraft.category, "A7") == 0 || aircraft.category[0] == 'B';
  return can_hover || !aircraft.motion_valid ||
         aircraft.gs_knots >= kSlowestFlightKnots;
}

const AircraftView* focusAircraft() {
  const AircraftView* selected = selectedAircraft();
  if (selected != nullptr) {
    return selected;
  }
  // s_visible is kept nearest first.
  for (size_t i = 0; i < s_visible_count; ++i) {
    if (isFlying(s_visible[i].aircraft)) {
      return &s_visible[i];
    }
  }
  return nullptr;
}

HistoryView historyFor(const char* hex) {
  static HistoryPoint ordered[kHistoryPointCount];
  HistorySlot* slot = findHistory(hex, false);
  if (slot == nullptr) {
    return {nullptr, 0};
  }
  for (size_t i = 0; i < slot->count; ++i) {
    ordered[i] = slot->points[(slot->start + i) % kHistoryPointCount];
  }
  return {ordered, slot->count};
}

const Alert& currentAlert() { return s_alert; }

}  // namespace crowpanel::model
