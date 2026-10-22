/* Added by the Sommelier project, 2026-10-20: frame statistics for Sommelier's compatibility reports;
   2026-10-22: written on a timer from the swap chain's creation, so a hung game is told apart (LGPL-2.1-or-later). */
#include "d3d11_frame_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <cstdio>
#include <windows.h>

namespace dxmt {

static constexpr auto kSkip = std::chrono::seconds(40);   // loading and shader building
static constexpr auto kWriteEvery = std::chrono::seconds(10);

FrameStats &
FrameStats::instance() {
  static FrameStats stats;
  return stats;
}

FrameStats::FrameStats() {
  std::string dir = env::getEnvVar("SOMMELIER_FRAME_STATS_DIR");
  if (dir.empty())
    return;
  if (dir.back() != '\\' && dir.back() != '/')
    dir += '\\';
  exe_ = env::getExeName();
  path_ = dir + str::format(exe_, "-", (unsigned int)GetCurrentProcessId(), ".txt");
  histogram_.assign(kBins, 0);
  enabled_ = true;
  Logger::info(str::format("Frame statistics: writing to ", path_));
}

void
FrameStats::frame(uint32_t width, uint32_t height) {
  if (!enabled_)
    return;
  auto now = clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (first_ == clock::time_point{}) {
    first_ = last_ = last_write_ = now;
    return;
  }
  double ms = std::chrono::duration<double, std::milli>(now - last_).count();
  last_ = now;
  width_ = width;
  height_ = height;
  presented_ += 1;
  if (now - first_ < kSkip || ms <= 0 || ms >= 1000)
    return;
  uint32_t bin = (uint32_t)(ms / kBinMs);
  histogram_[bin < kBins ? bin : kBins - 1] += 1;
  frames_ += 1;
  seconds_ += ms / 1000;
  if (now - last_write_ >= kWriteEvery) {
    last_write_ = now;
    write();
  }
}

double
FrameStats::percentileMs(const std::vector<uint32_t> &histogram, uint64_t count, double fraction) {
  if (!count)
    return 0;
  uint64_t target = (uint64_t)(count * fraction);
  uint64_t seen = 0;
  for (size_t i = 0; i < histogram.size(); i++) {
    seen += histogram[i];
    if (seen > target)
      return (i + 0.5) * kBinMs;
  }
  return histogram.size() * kBinMs;
}

/* The file's text from the counters as they are; `mutex_` must not be held. */
std::string
FrameStats::text() {
  std::lock_guard<std::mutex> lock(mutex_);
  double average = seconds_ > 0 ? frames_ / seconds_ : 0;
  double slow = percentileMs(histogram_, frames_, 0.99);
  double low1 = slow > 0 ? 1000.0 / slow : 0;
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "exe=%s\nframes=%llu\nseconds=%.1f\naverage=%.1f\nlow1=%.1f\nwidth=%u\nheight=%u\npresented=%llu\n",
           exe_.c_str(), (unsigned long long)frames_, seconds_, average, low1, width_, height_, (unsigned long long)presented_);
  return buffer;
}

bool
FrameStats::startWriter() {
  if (writer_started_)
    return true;
  writer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE thread = writer_event_ ? CreateThread(nullptr, 0, writerThread, this, 0, nullptr) : nullptr;
  if (!thread) {
    enabled_ = false;              // no thread, no statistics: never write on the presenting thread
    return false;
  }
  CloseHandle(thread);
  writer_started_ = true;
  return true;
}

void
FrameStats::swapChainCreated() {
  if (!enabled_)
    return;
  heartbeat_ = true;
  startWriter();
}

/* Called with `mutex_` held, from the presenting thread: only hands the writer a signal. */
void
FrameStats::write() {
  if (!frames_ || seconds_ <= 0)
    return;
  if (startWriter())
    SetEvent(writer_event_);
}

DWORD WINAPI
FrameStats::writerThread(void *self) {
  auto &stats = *static_cast<FrameStats *>(self);
  for (;;) {
    DWORD waited = WaitForSingleObject(stats.writer_event_, stats.heartbeat_ ? 10000 : INFINITE);
    if (waited == WAIT_TIMEOUT && !stats.heartbeat_)
      continue;
    std::string text = stats.text();
    FILE *file = fopen(stats.path_.c_str(), "w");
    if (!file)
      continue;
    fputs(text.c_str(), file);
    fclose(file);
  }
  return 0;
}

} // namespace dxmt
