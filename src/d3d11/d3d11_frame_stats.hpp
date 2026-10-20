/* Added by the Sommelier project, 2026-10-20: frame statistics for Sommelier's compatibility reports
   (LGPL-2.1-or-later). */
#pragma once
/*
 * Frame statistics: how fast the game ran, measured quietly (nothing is drawn). Every presented frame's time goes
 * into a histogram; every 10 seconds the totals are written, so they survive a crash:
 *
 *   SOMMELIER_FRAME_STATS_DIR\<exe>-<pid>.txt     (a Windows path, e.g. Z:\Users\me\...\FrameStats)
 *
 *   exe=Game.exe
 *   frames=12345
 *   seconds=210.5
 *   average=58.6
 *   low1=31.2
 *   width=2560
 *   height=1440
 *
 * `average` is frames per second over the time measured; `low1` is the frame rate of the slowest 1% of frames.
 * The first 20 seconds (loading, shader building) and gaps of a second or more (loading screens, alt-tab) are left
 * out. Off unless SOMMELIER_FRAME_STATS_DIR is set.
 */
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dxmt {

class FrameStats {
public:
  static FrameStats &instance();

  /* Called once per presented frame, with the size the game rendered at. */
  void frame(uint32_t width, uint32_t height);

  /* Pure helpers, kept separate so they can be checked on their own. */
  static double percentileMs(const std::vector<uint32_t> &histogram, uint64_t count, double fraction);

  static constexpr double kBinMs = 0.25;
  static constexpr uint32_t kBins = 4000;           // up to 1 second

private:
  FrameStats();
  void write();

  using clock = std::chrono::steady_clock;
  bool enabled_ = false;
  std::string path_;
  std::string exe_;
  std::mutex mutex_;
  clock::time_point first_{};
  clock::time_point last_{};
  clock::time_point last_write_{};
  std::vector<uint32_t> histogram_;
  uint64_t frames_ = 0;
  double seconds_ = 0;
  uint32_t width_ = 0, height_ = 0;
};

} // namespace dxmt
