#include "crowpanel/local_clock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "crowpanel/settings.h"
#include "crowpanel/time_zones.h"
#include "services/radar_location.h"

namespace crowpanel::local_clock {
namespace {

// Before this the clock has not been set from the network and reads 1970.
constexpr time_t kClockIsSetAfter = 1700000000;

// Puts the C library on a rule, unless it is on it already. The library keeps
// a copy of every different rule it is given, so it is not given one twice.
void useRule(const char* wanted) {
  const char* in_force = getenv("TZ");
  if (in_force != nullptr && strcmp(in_force, wanted) == 0) {
    return;
  }
  setenv("TZ", wanted, 1);
  tzset();
}

}  // namespace

const char* rule() {
  const settings::State& state = settings::get();
  switch (state.clock_zone) {
    case settings::ClockZone::Utc:
      return time_zones::kUtcRule;
    case settings::ClockZone::Chosen:
      return state.clock_rule[0] != '\0' ? state.clock_rule : time_zones::kUtcRule;
    case settings::ClockZone::RadarLocation:
    default:
      return time_zones::ruleAt(static_cast<float>(services::location::lat()),
                                static_cast<float>(services::location::lon()));
  }
}

void format(char* text, size_t length) {
  if (length == 0) {
    return;
  }
  text[0] = '\0';
  const time_t now = time(nullptr);
  if (now <= kClockIsSetAfter) {
    return;
  }
  // Looked up afresh each time, which is what makes the clock follow the
  // settings and the radar's centre without being told when they change.
  useRule(rule());
  struct tm local = {};
  localtime_r(&now, &local);

  const settings::State& state = settings::get();
  const bool utc = state.clock_zone == settings::ClockZone::Utc;
  char zone[12] = "UTC";
  if (!utc) {
    strftime(zone, sizeof(zone), "%Z", &local);
  }
  if (!state.clock_24_hour) {
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    snprintf(text, length, "%d:%02d %s %s", hour, local.tm_min,
             local.tm_hour < 12 ? "AM" : "PM", zone);
  } else if (utc) {
    snprintf(text, length, "%02d:%02dZ", local.tm_hour, local.tm_min);
  } else {
    snprintf(text, length, "%02d:%02d %s", local.tm_hour, local.tm_min, zone);
  }
}

}  // namespace crowpanel::local_clock
