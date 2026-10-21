/* Added by the Sommelier project, 2026-10-22: the cellar, pipelines built ahead of time (LGPL-2.1-or-later). */
#pragma once
/*
 * The cellar: Metal binary archives holding this game's pipelines, built ahead of time for this GPU and this
 * macOS, by Sommelier's service from other players' records or by Sommelier on this Mac. With archives open,
 * Metal takes a pipeline from them instead of compiling it; what they lack is compiled as usual, so a stale or
 * partial archive costs nothing.
 *
 * Files: `dxmt/<exe>/cellar/*.metallib` under the user's cache folder (or DXMT_CELLAR_PATH, a unix path).
 * DXMT_CELLAR=0 leaves them unopened.
 */
#include "Metal.hpp"
#include "thread.hpp"
#include "winemetal.h"
#include <algorithm>
#include <vector>

namespace dxmt {

class Cellar {
public:
  static Cellar &getInstance(WMTMetalVersion version);

  Cellar(WMTMetalVersion version) : version_(version) {}
  Cellar(const Cellar &) = delete;

  /* Opens the archives, once, with the first device that asks. */
  void open(WMT::Device device);

  /* Points a pipeline description at the archives (nothing when there are none). */
  template <typename Info>
  void
  lookup(Info &info) {
    if (handles_.empty())
      return;
    info.binary_archives_for_lookup.set(handles_.data());
    info.num_binary_archives_for_lookup = (uint8_t)std::min<size_t>(handles_.size(), 255);
    info.fail_on_binary_archive_miss = false;
  }

  size_t count() const { return handles_.size(); }

private:
  WMTMetalVersion version_;
  dxmt::mutex mutex_;
  bool opened_ = false;
  std::vector<WMT::Reference<WMT::BinaryArchive>> archives_;
  std::vector<obj_handle_t> handles_;
};

} // namespace dxmt
