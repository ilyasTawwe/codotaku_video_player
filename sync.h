#pragma once

#include <cmath>

extern "C" {
#include <libavutil/time.h>
}

// Wall-clock time in seconds, matching ffplay's use of av_gettime_relative().
inline double now_s() {
  return av_gettime_relative() / 1e6;
}

// A media clock that advances with wall time once set, modeled on ffplay's
// Clock. A clock that was never set (serial < 0) reads as NAN.
struct MediaClock {
  double pts = NAN;        // clock base
  double pts_drift = NAN;  // pts minus time at which we updated the clock
  double last_updated = NAN;
  double speed = 1.0;
  int serial = -1;
  bool paused = false;
};

inline double clock_get(const MediaClock &c) {
  if (c.serial < 0 || std::isnan(c.pts))
    return NAN;
  if (c.paused)
    return c.pts;
  double time = now_s();
  return c.pts_drift + time - (time - c.last_updated) * (1.0 - c.speed);
}

inline void clock_set_at(MediaClock &c, double pts, int serial, double time) {
  c.pts = pts;
  c.last_updated = time;
  c.pts_drift = c.pts - time;
  c.serial = serial;
}

inline void clock_set(MediaClock &c, double pts, int serial) {
  clock_set_at(c, pts, serial, now_s());
}
