#pragma once

#include <algorithm>
#include <cstdint>

namespace services {

/**
 * Works out how old a feed snapshot already was when its reply arrived.
 *
 * The feed takes a snapshot every couple of seconds and stamps it with its
 * own clock. A poll can be answered with one that is nearly that old, or with
 * the same one twice, so when a reply arrived says little about when its
 * positions were true. Treating every reply as fresh makes projected aircraft
 * jump back each time one is not.
 */
class SnapshotClock {
 public:
  /**
   * feed_ms: the snapshot's own timestamp, milliseconds since the epoch, or 0
   *   if the reply had none.
   * arrived_ms: when the reply arrived, on a clock of this device that never
   *   jumps.
   * wall_ms: this device's time of day at that moment, milliseconds since the
   *   epoch, or 0 if it has not been set.
   * Returns the snapshot's age on arrival, in milliseconds.
   */
  uint32_t ageOnArrival(uint64_t feed_ms, int64_t arrived_ms, uint64_t wall_ms) {
    if (feed_ms == 0) {
      return 0;
    }
    // A snapshot cannot arrive before it was taken, so the freshest reply
    // seen shows how the feed's clock lines up with this device's, to within
    // that reply's own age. The mark creeps so that clocks drifting apart do
    // not leave it behind.
    const int64_t gap = arrived_ms - static_cast<int64_t>(feed_ms);
    alignment_ = aligned_ ? std::min(gap, alignment_ + kCreepMs) : gap;
    aligned_ = true;

    // A set clock lines the two up exactly. Believe it only while it does not
    // put a snapshot after its own arrival or implausibly long before; a clock
    // that is off would bring the jumps back.
    int64_t lined_up = alignment_;
    if (wall_ms != 0) {
      const int64_t by_clock = arrived_ms - static_cast<int64_t>(wall_ms);
      if (by_clock < alignment_ && by_clock >= alignment_ - kLongestGapMs) {
        lined_up = by_clock;
      }
    }
    return static_cast<uint32_t>(gap - lined_up);
  }

 private:
  static constexpr int64_t kCreepMs = 1;
  /** No feed is expected to go longer than this between snapshots. */
  static constexpr int64_t kLongestGapMs = 5000;

  bool aligned_ = false;
  int64_t alignment_ = 0;
};

}  // namespace services
