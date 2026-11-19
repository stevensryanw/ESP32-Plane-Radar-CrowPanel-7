#pragma once

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();
double userLat();
double userLon();

/** True when startup should choose the best airport near the saved user location. */
bool needsDefaultAirport();
bool hasSelectedAirport();
const char* selectedAirport();

/** Persist an airport-centered radar location. */
void selectAirport(const char* ident, double lat, double lon);
/** Center on the exact saved user coordinates instead of an airport. */
void useUserLocation();

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);
/** Same as saveFromStrings for coordinates that are already numeric. */
bool saveUserLocation(double lat, double lon);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
