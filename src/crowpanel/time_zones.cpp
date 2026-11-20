#include "crowpanel/time_zones.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crowpanel/time_zone_data.h"

namespace crowpanel::time_zones {

const char* ruleAt(float lat, float lon) {
  using namespace time_zone_data;
  if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0f || lat > 90.0f ||
      lon < -180.0f || lon > 180.0f) {
    return kUtcRule;
  }
  const int row = std::min(
      kMapRows - 1, static_cast<int>((90.0f - lat) * kCellsPerDegree));
  const int column = std::min(
      kMapColumns - 1, static_cast<int>((lon + 180.0f) * kCellsPerDegree));

  // The last run in the row that starts at or before the column. Every row's
  // first run starts at column 0, so there always is one.
  const uint16_t* const first = kRunColumn + kRowFirstRun[row];
  const uint16_t* const end = kRunColumn + kRowFirstRun[row + 1];
  const uint16_t* const after =
      std::upper_bound(first, end, static_cast<uint16_t>(column));
  if (after == first) {
    return kUtcRule;
  }
  const uint8_t rule = kRunRule[(after - 1) - kRunColumn];
  return rule < kRuleCount ? kRules[rule] : kUtcRule;
}

int choiceForRule(const char* rule) {
  using namespace time_zone_data;
  if (rule == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < kChoiceCount; ++i) {
    if (strcmp(kRules[kChoices[i].rule], rule) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace crowpanel::time_zones
