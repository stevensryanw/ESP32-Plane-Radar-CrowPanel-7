#pragma once

namespace crowpanel::geo {

struct Offset {
  float east_km;
  float north_km;
  float distance_km;
  float bearing_deg;
};

Offset offsetFrom(double center_lat, double center_lon, double lat, double lon);

/** Where a target at lat/lon is after `seconds` on `track_deg` at `speed_knots`. */
void advance(double lat, double lon, float track_deg, float speed_knots,
             float seconds, float* advanced_lat, float* advanced_lon);

/** Where a target that holds its track comes closest to the origin. */
struct Approach {
  /** False once the target is at or past its closest point. */
  bool closing;
  /** Until the closest point; 0 when not closing. */
  float seconds;
  /** Distance from the origin at the closest point. */
  float distance_km;
  /** Bearing from the origin to the closest point. */
  float bearing_deg;
};

/**
 * Closest approach to the origin of a target now `distance_km` away on
 * `bearing_deg`, moving along `track_deg` at `speed_knots`.
 */
Approach closestApproach(float distance_km, float bearing_deg, float track_deg,
                         float speed_knots);

}  // namespace crowpanel::geo
