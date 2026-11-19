#include "crowpanel/settings.h"

#include <Preferences.h>

#include <algorithm>
#include <iterator>

namespace crowpanel::settings {
namespace {

constexpr char kNamespace[] = "cpui";
State s_state;

bool validRefresh(uint8_t seconds) {
  return std::find(std::begin(kRefreshChoices), std::end(kRefreshChoices),
                   seconds) != std::end(kRefreshChoices);
}

template <typename Writer>
void writePreference(Writer writer) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    return;
  }
  writer(prefs);
  prefs.end();
}

}  // namespace

void init() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {
    return;
  }
  s_state.show_labels = prefs.getBool("labels", true);
  s_state.show_trails = prefs.getBool("trails", true);
  s_state.show_ground = prefs.getBool("ground", false);
  s_state.logging_enabled = prefs.getBool("logging", false);
  s_state.alerts_enabled = prefs.getBool("alerts", true);
  s_state.brightness_percent =
      std::min<uint8_t>(100, prefs.getUChar("bright", 75));
  const uint8_t refresh = prefs.getUChar("refresh", kDefaultRefreshSeconds);
  s_state.refresh_seconds =
      validRefresh(refresh) ? refresh : kDefaultRefreshSeconds;
  s_state.proximity_km =
      std::max<uint8_t>(1, std::min<uint8_t>(25, prefs.getUChar("prox", 5)));
  const uint8_t band = prefs.getUChar("altBand", 0);
  s_state.altitude_band = band <= static_cast<uint8_t>(AltitudeBand::Above30000)
                              ? static_cast<AltitudeBand>(band)
                              : AltitudeBand::All;
  const uint8_t theme = prefs.getUChar("theme", 0);
  s_state.theme = theme <= static_cast<uint8_t>(Theme::HighContrast)
                      ? static_cast<Theme>(theme)
                      : Theme::Night;
  prefs.end();
}

const State& get() { return s_state; }

void setShowLabels(bool value) {
  s_state.show_labels = value;
  writePreference([&](Preferences& p) { p.putBool("labels", value); });
}

void setShowTrails(bool value) {
  s_state.show_trails = value;
  writePreference([&](Preferences& p) { p.putBool("trails", value); });
}

void setShowGround(bool value) {
  s_state.show_ground = value;
  writePreference([&](Preferences& p) { p.putBool("ground", value); });
}

void setLoggingEnabled(bool value) {
  s_state.logging_enabled = value;
  writePreference([&](Preferences& p) { p.putBool("logging", value); });
}

void setAlertsEnabled(bool value) {
  s_state.alerts_enabled = value;
  writePreference([&](Preferences& p) { p.putBool("alerts", value); });
}

void setBrightness(uint8_t percent) {
  s_state.brightness_percent = std::min<uint8_t>(percent, 100);
  writePreference(
      [&](Preferences& p) { p.putUChar("bright", s_state.brightness_percent); });
}

void setRefreshSeconds(uint8_t seconds) {
  if (!validRefresh(seconds)) {
    return;
  }
  s_state.refresh_seconds = seconds;
  writePreference([&](Preferences& p) { p.putUChar("refresh", seconds); });
}

void setProximityKm(uint8_t km) {
  s_state.proximity_km = std::max<uint8_t>(1, std::min<uint8_t>(25, km));
  writePreference([&](Preferences& p) { p.putUChar("prox", s_state.proximity_km); });
}

void setAltitudeBand(AltitudeBand band) {
  s_state.altitude_band = band;
  writePreference(
      [&](Preferences& p) { p.putUChar("altBand", static_cast<uint8_t>(band)); });
}

void setTheme(Theme theme) {
  s_state.theme = theme;
  writePreference(
      [&](Preferences& p) { p.putUChar("theme", static_cast<uint8_t>(theme)); });
}

bool aircraftAltitudeVisible(bool on_ground, bool altitude_valid,
                             int32_t altitude_ft) {
  if (on_ground) {
    return s_state.show_ground;
  }
  if (!altitude_valid || s_state.altitude_band == AltitudeBand::All) {
    return true;
  }
  switch (s_state.altitude_band) {
    case AltitudeBand::Below10000:
      return altitude_ft < 10000;
    case AltitudeBand::From10000To30000:
      return altitude_ft >= 10000 && altitude_ft <= 30000;
    case AltitudeBand::Above30000:
      return altitude_ft > 30000;
    case AltitudeBand::All:
    default:
      return true;
  }
}

}  // namespace crowpanel::settings
