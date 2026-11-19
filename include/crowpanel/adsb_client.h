#pragma once

#include <cstddef>
#include <cstdint>

// The 7-inch panel's aircraft feed client. It has the same namespace and
// function names as services/adsb_client.h, which the round display uses,
// but its own Aircraft record and its own way of fetching. A build has one
// or the other, never both: platformio.ini leaves the other's source out.
namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  float seen_sec;
  /** Seconds between the position report and the snapshot that carried it. */
  float position_age_sec;
  int32_t alt_ft;
  int32_t vertical_rate_fpm;
  bool altitude_valid;
  bool on_ground;
  bool emergency;
  /** track_deg and gs_knots were reported, so the position can be projected. */
  bool motion_valid;
  /** Flagged as military in the feed's aircraft database. */
  bool military;
  char hex[7];
  char callsign[9];
  char type[5];
  char alt[12];
  char registration[12];
  char category[5];
  char squawk[5];
  char description[36];
};

constexpr size_t kMaxAircraft = 256;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/**
 * Drop the connection kept open between fetches, releasing its TLS memory.
 * The next fetch reconnects. Call before opening another TLS connection.
 */
void closeConnection();

/** Millis timestamp of the last successfully parsed API response. */
unsigned long lastSuccessMs();
/**
 * Millis timestamp at which the feed took the snapshot now in aircraftList().
 * Earlier than lastSuccessMs(): the feed takes one every couple of seconds, so
 * a reply can carry a snapshot that old, or the same one as the reply before.
 */
unsigned long snapshotMs();
/** Millis timestamp of the most recent request attempt. */
unsigned long lastAttemptMs();
int lastHttpStatus();
const char* lastError();

}  // namespace services::adsb
