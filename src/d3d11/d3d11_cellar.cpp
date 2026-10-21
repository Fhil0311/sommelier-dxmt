/* Added by the Sommelier project, 2026-10-22: the cellar, pipelines built ahead of time (LGPL-2.1-or-later). */
#include "d3d11_cellar.hpp"
#include "d3d11_pipeline_records.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <windows.h>

namespace dxmt {

Cellar &
Cellar::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<unsigned int, std::unique_ptr<Cellar>> instances;
  std::lock_guard<dxmt::mutex> lock(mutex);
  auto &instance = instances[(unsigned int)version];
  if (!instance)
    instance = std::make_unique<Cellar>(version);
  return *instance;
}

void
Cellar::open(WMT::Device device) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (opened_)
    return;
  opened_ = true;
  if (env::getEnvVar("DXMT_CELLAR") == "0")
    return;
  std::string folder = env::getEnvVar("DXMT_CELLAR_PATH");
  if (folder.empty() || folder[0] != '/') {
    folder = PipelineCacheFolderUnix();
    if (folder.empty())
      return;
    folder += "cellar/";
  }
  if (folder.back() != '/')
    folder += '/';

  /* Listed through Wine's Z: drive, which is the unix tree; opened by their unix paths. */
  std::string pattern = "Z:";
  for (char c : folder)
    pattern += c == '/' ? '\\' : c;
  pattern += "*.metallib";
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA(pattern.c_str(), &found);
  if (search == INVALID_HANDLE_VALUE)
    return;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    std::string path = folder + found.cFileName;
    WMT::Reference<WMT::Error> err;
    auto archive = device.newBinaryArchive(path.c_str(), err);
    if (!archive || err) {
      WARN("Cellar: can't open ", path, ": ", err ? err.description().getUTF8String() : "no archive");
      continue;
    }
    handles_.push_back(archive.handle);
    archives_.push_back(std::move(archive));
  } while (FindNextFileA(search, &found));
  FindClose(search);
  if (!handles_.empty())
    Logger::info(str::format("Cellar: ", handles_.size(), " archives open in ", folder));
}

} // namespace dxmt
