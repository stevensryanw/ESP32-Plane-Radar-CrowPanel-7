#pragma once

#include <cstddef>
#include <cstdint>

namespace crowpanel::settings {

enum class AltitudeBand : uint8_t {
  All = 0,
  Below10000,
  From10000To30000,
  Above30000,
};

enum class Theme : uint8_t {
  Night = 0,
  Green,
  HighContrast,
};

/** Whose time the clock in the status strip keeps. */
enum class ClockZone : uint8_t {
  RadarLocation = 0,  // Wherever the radar is centred.
  Utc,
  Chosen,  // State::clock_rule, picked from the list on the settings screen.
};

/** Room for a time-zone rule (a POSIX TZ string) and the end of its string. */
constexpr size_t kClockRuleLength = 48;

/**
 * Seconds between ADS-B fetches the user can choose from. adsb.fi takes a new
 * snapshot every 2 s, so anything shorter only fetches, and redraws, the same
 * data twice.
 */
constexpr uint8_t kRefreshChoices[] = {2, 3, 5, 10};
constexpr uint8_t kDefaultRefreshSeconds = 2;

struct State {
  bool show_labels = true;
  bool show_trails = true;
  bool show_ground = false;
  bool logging_enabled = false;
  bool alerts_enabled = true;
  uint8_t brightness_percent = 75;
  uint8_t refresh_seconds = kDefaultRefreshSeconds;
  uint8_t proximity_km = 5;
  AltitudeBand altitude_band = AltitudeBand::All;
  Theme theme = Theme::Night;
  ClockZone clock_zone = ClockZone::RadarLocation;
  char clock_rule[kClockRuleLength] = "";  // Used while clock_zone is Chosen.
  bool clock_24_hour = true;
};

void init();
const State& get();

void setShowLabels(bool value);
void setShowTrails(bool value);
void setShowGround(bool value);
void setLoggingEnabled(bool value);
void setAlertsEnabled(bool value);
void setBrightness(uint8_t percent);
void setRefreshSeconds(uint8_t seconds);
void setProximityKm(uint8_t km);
void setAltitudeBand(AltitudeBand band);
void setTheme(Theme theme);
/** For RadarLocation and Utc. Choosing a zone by name is setClockRule(). */
void setClockZone(ClockZone zone);
/** Keeps the clock on one zone; ignores a rule the settings screen does not list. */
void setClockRule(const char* rule);
void setClock24Hour(bool value);

bool aircraftAltitudeVisible(bool on_ground, bool altitude_valid, int32_t altitude_ft);

}  // namespace crowpanel::settings
