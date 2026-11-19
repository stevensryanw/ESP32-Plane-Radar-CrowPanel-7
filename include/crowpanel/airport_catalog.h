#pragma once

#include <cstddef>
#include <cstdint>

namespace data::airport_catalog {

struct Match {
  uint16_t airport_index;
  float distance_km;
};

/**
 * Latitude of every airport in hundredths of a degree, copied to RAM on first
 * use, or null if that copy could not be allocated. The table itself is in
 * flash, which shares its bus with the display's frame buffer: reading all of
 * it starves the panel for a moment and shows as a displaced frame. Range
 * queries test this copy first and only touch the entries that are close.
 */
const int16_t* latitudeCells();

/** Inclusive range of latitudeCells() values around a latitude. */
struct LatitudeBand {
  int16_t low;
  int16_t high;
  bool contains(int16_t cell) const { return cell >= low && cell <= high; }
};
LatitudeBand latitudeBand(double lat, float margin_km);

/** Human-readable airport name/municipality stored in the generated catalog. */
const char* name(size_t airport_index);

/** Great-circle distance between two coordinates. */
float distanceKm(double center_lat, double center_lon, double lat, double lon);

/** Search identifiers and airport names, ranked by match quality and relevance. */
size_t search(const char* query, double near_lat, double near_lon, Match* output,
              size_t capacity);

/** Return the nearest airports, ordered strictly by distance. */
size_t nearest(double lat, double lon, Match* output, size_t capacity,
               float maximum_km = 200.0f);

/** Pick a useful default airport near a location, balancing size and distance. */
int bestNearby(double lat, double lon, float maximum_km = 80.0f);

}  // namespace data::airport_catalog
