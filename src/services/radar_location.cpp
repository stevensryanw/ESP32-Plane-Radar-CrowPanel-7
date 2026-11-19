#include "services/radar_location.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyCenterMode[] = "centerMode";
constexpr char kKeyAirport[] = "airport";
constexpr char kKeyAirportLat[] = "airportLat";
constexpr char kKeyAirportLon[] = "airportLon";

constexpr uint8_t kCenterAutomatic = 0;
constexpr uint8_t kCenterUserLocation = 1;
constexpr uint8_t kCenterAirport = 2;

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
double s_user_lat = config::kDefaultRadarLat;
double s_user_lon = config::kDefaultRadarLon;
uint8_t s_center_mode = kCenterAutomatic;
char s_airport[5] = {};

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void persistUserLocation(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
#ifdef PLANE_RADAR_CROWPANEL
  // A new location clears the panel's choice of home airport, so that the
  // nearest one is picked again. The round display has no such choice.
  prefs.putUChar(kKeyCenterMode, kCenterAutomatic);
  prefs.remove(kKeyAirport);
  prefs.remove(kKeyAirportLat);
  prefs.remove(kKeyAirportLon);
#endif
  prefs.end();
  s_user_lat = lat;
  s_user_lon = lon;
  s_lat = lat;
  s_lon = lon;
  s_center_mode = kCenterAutomatic;
  s_airport[0] = '\0';
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_user_lat = lat;
      s_user_lon = lon;
    }
  }
  s_center_mode = prefs.getUChar(kKeyCenterMode, kCenterAutomatic);
  if (s_center_mode == kCenterAirport && prefs.isKey(kKeyAirportLat) &&
      prefs.isKey(kKeyAirportLon)) {
    const double airport_lat = prefs.getDouble(kKeyAirportLat, s_user_lat);
    const double airport_lon = prefs.getDouble(kKeyAirportLon, s_user_lon);
    const String airport = prefs.getString(kKeyAirport, "");
    if (validLatLon(airport_lat, airport_lon) && airport.length() > 0) {
      s_lat = airport_lat;
      s_lon = airport_lon;
      strncpy(s_airport, airport.c_str(), sizeof(s_airport) - 1);
    } else {
      s_center_mode = kCenterAutomatic;
    }
  }
  if (s_center_mode != kCenterAirport) {
    s_lat = s_user_lat;
    s_lon = s_user_lon;
  }
  prefs.end();
}

double lat() { return s_lat; }

double lon() { return s_lon; }

double userLat() { return s_user_lat; }

double userLon() { return s_user_lon; }

bool needsDefaultAirport() { return s_center_mode == kCenterAutomatic; }

bool hasSelectedAirport() {
  return s_center_mode == kCenterAirport && s_airport[0] != '\0';
}

const char* selectedAirport() { return s_airport; }

void selectAirport(const char* ident, double lat, double lon) {
  if (ident == nullptr || ident[0] == '\0' || !validLatLon(lat, lon)) {
    return;
  }
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putUChar(kKeyCenterMode, kCenterAirport);
  prefs.putString(kKeyAirport, ident);
  prefs.putDouble(kKeyAirportLat, lat);
  prefs.putDouble(kKeyAirportLon, lon);
  prefs.end();
  s_center_mode = kCenterAirport;
  strncpy(s_airport, ident, sizeof(s_airport) - 1);
  s_airport[sizeof(s_airport) - 1] = '\0';
  s_lat = lat;
  s_lon = lon;
  Serial.printf("Radar centered on airport %s: %.6f, %.6f\n", s_airport, lat,
                lon);
}

void useUserLocation() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putUChar(kKeyCenterMode, kCenterUserLocation);
  prefs.remove(kKeyAirport);
  prefs.remove(kKeyAirportLat);
  prefs.remove(kKeyAirportLon);
  prefs.end();
  s_center_mode = kCenterUserLocation;
  s_airport[0] = '\0';
  s_lat = s_user_lat;
  s_lon = s_user_lon;
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  return saveUserLocation(lat, lon);
}

bool saveUserLocation(double lat, double lon) {
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persistUserLocation(lat, lon);
  Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyCenterMode);
  prefs.remove(kKeyAirport);
  prefs.remove(kKeyAirportLat);
  prefs.remove(kKeyAirportLon);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_user_lat = config::kDefaultRadarLat;
  s_user_lon = config::kDefaultRadarLon;
  s_center_mode = kCenterAutomatic;
  s_airport[0] = '\0';
}

}  // namespace services::location
