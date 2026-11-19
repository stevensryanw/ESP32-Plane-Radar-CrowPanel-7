#include "crowpanel/geo.h"

#include <algorithm>
#include <cmath>

namespace crowpanel::geo {

namespace {
constexpr double kKmPerDegree = 111.195;
constexpr double kDegToRad = 0.017453292519943295;
constexpr double kKmPerSecondPerKnot = 1.852 / 3600.0;
}  // namespace

Offset offsetFrom(double center_lat, double center_lon, double lat, double lon) {
  const double north = (lat - center_lat) * kKmPerDegree;
  const double east =
      (lon - center_lon) * kKmPerDegree * cos(center_lat * kDegToRad);
  const double distance = sqrt(east * east + north * north);
  double bearing = atan2(east, north) / kDegToRad;
  if (bearing < 0.0) {
    bearing += 360.0;
  }
  return {static_cast<float>(east), static_cast<float>(north),
          static_cast<float>(distance), static_cast<float>(bearing)};
}

void advance(double lat, double lon, float track_deg, float speed_knots,
             float seconds, float* advanced_lat, float* advanced_lon) {
  const double travelled_km = speed_knots * kKmPerSecondPerKnot * seconds;
  const double track = track_deg * kDegToRad;
  // Away from the poles; the same flat-earth scale offsetFrom() uses.
  const double east_scale = std::max(0.01, cos(lat * kDegToRad));
  *advanced_lat = static_cast<float>(lat + travelled_km * cos(track) / kKmPerDegree);
  *advanced_lon = static_cast<float>(
      lon + travelled_km * sin(track) / (kKmPerDegree * east_scale));
}

Approach closestApproach(float distance_km, float bearing_deg, float track_deg,
                         float speed_knots) {
  constexpr float kRadiansPerDegree = 0.01745329252f;
  constexpr float kKmPerSecondPerKnot = 1.852f / 3600.0f;
  const float east = distance_km * sinf(bearing_deg * kRadiansPerDegree);
  const float north = distance_km * cosf(bearing_deg * kRadiansPerDegree);
  const float speed = speed_knots * kKmPerSecondPerKnot;
  const float east_speed = speed * sinf(track_deg * kRadiansPerDegree);
  const float north_speed = speed * cosf(track_deg * kRadiansPerDegree);

  // The position is closest where it is at right angles to the velocity.
  const float seconds = speed <= 0.0f
                            ? 0.0f
                            : -(east * east_speed + north * north_speed) /
                                  (speed * speed);
  // Less than a second away counts as there already: a target flying past
  // abeam is never exactly at right angles in floating point.
  if (seconds < 1.0f) {
    return {false, 0.0f, distance_km, bearing_deg};
  }
  const float closest_east = east + east_speed * seconds;
  const float closest_north = north + north_speed * seconds;
  float bearing = atan2f(closest_east, closest_north) / kRadiansPerDegree;
  if (bearing < 0.0f) {
    bearing += 360.0f;
  }
  return {true, seconds, sqrtf(closest_east * closest_east +
                               closest_north * closest_north), bearing};
}

}  // namespace crowpanel::geo
