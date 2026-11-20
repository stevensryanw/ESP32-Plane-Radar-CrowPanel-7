#pragma once

namespace crowpanel::time_zones {

/** The rule for UTC, which is also what the map gives for a position it cannot use. */
constexpr char kUtcRule[] = "UTC0";

/**
 * The time-zone rule in force at a position, as a POSIX TZ string that stays
 * valid for as long as the firmware runs.
 *
 * It comes from a map with quarter-degree cells, so within about 30 km of a
 * border between zones it can give the neighbouring one.
 */
const char* ruleAt(float lat, float lon);

/**
 * Where a rule is in time_zone_data::kChoices, the zones the settings screen
 * lists, or -1 if it is not one of them.
 */
int choiceForRule(const char* rule);

}  // namespace crowpanel::time_zones
