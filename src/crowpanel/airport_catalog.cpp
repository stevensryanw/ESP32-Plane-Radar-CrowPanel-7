#include "crowpanel/airport_catalog.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "crowpanel/airports.h"

namespace data::airport_catalog {
namespace {

constexpr double kEarthRadiusKm = 6371.0;
constexpr size_t kQueryLength = 48;
// A name search has to read every airport and name from flash. Pausing
// briefly every few entries leaves the shared bus free often enough that
// the display's frame buffer stream keeps up; see latitudeCells().
constexpr size_t kSearchSliceAirports = 32;
constexpr uint32_t kSearchSlicePauseUs = 250;

int16_t* s_latitude_cells = nullptr;
bool s_latitude_cells_built = false;

void uppercaseTrimmed(const char* source, char* destination, size_t length) {
  if (destination == nullptr || length == 0) {
    return;
  }
  while (source != nullptr && isspace(static_cast<unsigned char>(*source))) {
    ++source;
  }
  size_t output = 0;
  while (source != nullptr && *source != '\0' && output + 1 < length) {
    destination[output++] =
        static_cast<char>(toupper(static_cast<unsigned char>(*source++)));
  }
  while (output > 0 && isspace(static_cast<unsigned char>(destination[output - 1]))) {
    --output;
  }
  destination[output] = '\0';
}

bool startsWithInsensitive(const char* text, const char* query) {
  if (text == nullptr || query == nullptr) {
    return false;
  }
  for (size_t i = 0; query[i] != '\0'; ++i) {
    if (text[i] == '\0' ||
        toupper(static_cast<unsigned char>(text[i])) !=
            toupper(static_cast<unsigned char>(query[i]))) {
      return false;
    }
  }
  return true;
}

bool containsInsensitive(const char* text, const char* query) {
  if (text == nullptr || query == nullptr || query[0] == '\0') {
    return false;
  }
  for (size_t offset = 0; text[offset] != '\0'; ++offset) {
    if (startsWithInsensitive(text + offset, query)) {
      return true;
    }
  }
  return false;
}

const char* shortIdent(const char* ident) {
  return ident != nullptr && ident[0] == 'K' && strlen(ident) == 4 ? ident + 1
                                                                  : ident;
}

void insertByDistance(const Match& candidate, Match* output, size_t* count,
                      size_t capacity) {
  if (capacity == 0) {
    return;
  }
  size_t position = 0;
  while (position < *count && output[position].distance_km <= candidate.distance_km) {
    ++position;
  }
  if (position >= capacity) {
    return;
  }
  const size_t new_count = std::min(*count + 1, capacity);
  for (size_t i = new_count - 1; i > position; --i) {
    output[i] = output[i - 1];
  }
  output[position] = candidate;
  *count = new_count;
}

struct ScoredMatch {
  Match match;
  int score;
};

void insertByScore(const ScoredMatch& candidate, ScoredMatch* output,
                   size_t* count, size_t capacity) {
  if (capacity == 0) {
    return;
  }
  size_t position = 0;
  while (position < *count && output[position].score >= candidate.score) {
    ++position;
  }
  if (position >= capacity) {
    return;
  }
  const size_t new_count = std::min(*count + 1, capacity);
  for (size_t i = new_count - 1; i > position; --i) {
    output[i] = output[i - 1];
  }
  output[position] = candidate;
  *count = new_count;
}

}  // namespace

const int16_t* latitudeCells() {
  if (!s_latitude_cells_built) {
    s_latitude_cells_built = true;
    auto* cells = static_cast<int16_t*>(
        malloc(large_airports::kAirportCount * sizeof(int16_t)));
    if (cells != nullptr) {
      for (size_t i = 0; i < large_airports::kAirportCount; ++i) {
        cells[i] = static_cast<int16_t>(large_airports::kAirports[i].lat_e7 / 100000);
      }
      s_latitude_cells = cells;
    }
  }
  return s_latitude_cells;
}

LatitudeBand latitudeBand(double lat, float margin_km) {
  // 110.5 km per degree is the short end of the range, and one extra cell on
  // each side covers the truncation in latitudeCells().
  const double margin_deg = margin_km / 110.5;
  const double low = floor((lat - margin_deg) * 100.0) - 1.0;
  const double high = ceil((lat + margin_deg) * 100.0) + 1.0;
  return {static_cast<int16_t>(std::max(-9001.0, low)),
          static_cast<int16_t>(std::min(9001.0, high))};
}

const char* name(size_t airport_index) {
  if (airport_index >= large_airports::kAirportCount) {
    return "Unknown airport";
  }
  return large_airports::kAirportNames[airport_index];
}

float distanceKm(double center_lat, double center_lon, double lat, double lon) {
  constexpr double kDegToRad = 0.017453292519943295;
  const double lat1 = center_lat * kDegToRad;
  const double lat2 = lat * kDegToRad;
  const double delta_lat = (lat - center_lat) * kDegToRad;
  const double delta_lon = (lon - center_lon) * kDegToRad;
  const double a = sin(delta_lat * 0.5) * sin(delta_lat * 0.5) +
                   cos(lat1) * cos(lat2) * sin(delta_lon * 0.5) *
                       sin(delta_lon * 0.5);
  const double angle = 2.0 * atan2(sqrt(a), sqrt(std::max(0.0, 1.0 - a)));
  return static_cast<float>(kEarthRadiusKm * angle);
}

size_t nearest(double lat, double lon, Match* output, size_t capacity,
               float maximum_km) {
  if (output == nullptr || capacity == 0) {
    return 0;
  }
  size_t count = 0;
  const int16_t* cells = latitudeCells();
  const LatitudeBand band = latitudeBand(lat, maximum_km);
  for (size_t i = 0; i < large_airports::kAirportCount; ++i) {
    if (cells != nullptr && !band.contains(cells[i])) {
      continue;
    }
    const auto& airport = large_airports::kAirports[i];
    const float distance = distanceKm(lat, lon, airport.lat_e7 * 1e-7,
                                      airport.lon_e7 * 1e-7);
    if (distance <= maximum_km) {
      insertByDistance({static_cast<uint16_t>(i), distance}, output, &count,
                       capacity);
    }
  }
  return count;
}

size_t search(const char* query, double near_lat, double near_lon, Match* output,
              size_t capacity) {
  if (output == nullptr || capacity == 0) {
    return 0;
  }
  char normalized[kQueryLength];
  uppercaseTrimmed(query, normalized, sizeof(normalized));
  if (normalized[0] == '\0') {
    return 0;
  }

  ScoredMatch ranked[12] = {};
  const size_t ranked_capacity = std::min(capacity, sizeof(ranked) / sizeof(ranked[0]));
  size_t count = 0;
  for (size_t i = 0; i < large_airports::kAirportCount; ++i) {
    if (i % kSearchSliceAirports == kSearchSliceAirports - 1) {
      delayMicroseconds(kSearchSlicePauseUs);
    }
    const auto& airport = large_airports::kAirports[i];
    const char* ident = airport.ident;
    const char* familiar_ident = shortIdent(ident);
    const char* airport_name = name(i);
    int match_score = 0;
    if (strcmp(normalized, ident) == 0 || strcmp(normalized, familiar_ident) == 0) {
      match_score = 10000;
    } else if (startsWithInsensitive(ident, normalized) ||
               startsWithInsensitive(familiar_ident, normalized)) {
      match_score = 8000;
    } else if (startsWithInsensitive(airport_name, normalized)) {
      match_score = 6500;
    } else if (containsInsensitive(airport_name, normalized)) {
      match_score = 5000;
    } else {
      continue;
    }
    const float distance = distanceKm(near_lat, near_lon, airport.lat_e7 * 1e-7,
                                      airport.lon_e7 * 1e-7);
    const int score = match_score + airport.priority * 100 -
                      static_cast<int>(std::min(distance, 999.0f) * 0.1f);
    insertByScore({{static_cast<uint16_t>(i), distance}, score}, ranked, &count,
                  ranked_capacity);
  }
  for (size_t i = 0; i < count; ++i) {
    output[i] = ranked[i].match;
  }
  return count;
}

int bestNearby(double lat, double lon, float maximum_km) {
  int best_index = -1;
  float best_score = -100000.0f;
  float best_distance = maximum_km;
  const int16_t* cells = latitudeCells();
  const LatitudeBand band = latitudeBand(lat, maximum_km);
  for (size_t i = 0; i < large_airports::kAirportCount; ++i) {
    if (cells != nullptr && !band.contains(cells[i])) {
      continue;
    }
    const auto& airport = large_airports::kAirports[i];
    const float distance = distanceKm(lat, lon, airport.lat_e7 * 1e-7,
                                      airport.lon_e7 * 1e-7);
    if (distance > maximum_km) {
      continue;
    }
    const float runway_bonus =
        std::min<uint16_t>(airport.longest_runway_m, 3000) / 300.0f;
    const float score = airport.priority * 30.0f + runway_bonus - distance;
    if (score > best_score ||
        (fabsf(score - best_score) < 0.01f && distance < best_distance)) {
      best_index = static_cast<int>(i);
      best_score = score;
      best_distance = distance;
    }
  }
  return best_index;
}

}  // namespace data::airport_catalog
