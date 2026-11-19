#pragma once

#include <cstddef>
#include <cstdint>

#include "crowpanel/adsb_client.h"

namespace crowpanel::model {

constexpr size_t kHistoryPointCount = 24;
constexpr size_t kHistoryAircraftCount = 96;

struct AircraftView {
  /** As reported, except lat/lon, which advance() moves along the track. */
  services::adsb::Aircraft aircraft;
  /** The position as reported in the last snapshot. */
  float fix_lat;
  float fix_lon;
  float distance_km;
  float bearing_deg;
};

struct HistoryPoint {
  float lat;
  float lon;
  uint32_t timestamp_ms;
};

struct HistoryView {
  const HistoryPoint* points;
  size_t count;
};

struct Alert {
  bool active;
  bool emergency;
  char text[72];
};

/** Consume the current ADS-B snapshot, update trails, sort, and filter. */
void update();
/**
 * Move every visible aircraft to where its last report puts it at `now_ms`,
 * so targets glide between snapshots instead of jumping once per fetch.
 */
void advance(uint32_t now_ms);
/** Clear targets, trails, selection, and alerts after changing radar center. */
void reset();

const AircraftView* visibleAircraft();
size_t visibleAircraftCount();

void selectAircraft(const char* hex);
void clearSelection();
const char* selectedHex();
const AircraftView* selectedAircraft();
/**
 * The aircraft worth describing when nothing is selected: the selected one if
 * there is one, otherwise the nearest that is in the air.
 */
const AircraftView* focusAircraft();

HistoryView historyFor(const char* hex);
const Alert& currentAlert();

}  // namespace crowpanel::model
