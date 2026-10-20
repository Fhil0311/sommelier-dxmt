/* Added by the Sommelier project, 2026-10-20: pipeline records and warm-up (LGPL-2.1-or-later). */
#include "d3d11_pipeline_records.hpp"
#include "Metal.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace dxmt {

static constexpr char kMagic[4] = {'D', 'X', 'P', 'L'};

PipelineRecords &
PipelineRecords::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<unsigned int, std::unique_ptr<PipelineRecords>> instances;
  std::lock_guard<dxmt::mutex> lock(mutex);
  auto &instance = instances[(unsigned int)version];
  if (!instance)
    instance = std::make_unique<PipelineRecords>(version);
  return *instance;
}

/* The folder DXMT's caches live in, as a path this (Windows) side can open: Wine shows the unix tree as Z:. */
static std::string
cacheFolder() {
  std::string base = env::getEnvVar("DXMT_SHADER_CACHE_PATH");
  if (base.empty() || base[0] != '/') {
    char buffer[4096];
    uint64_t length = WMTGetUserCacheDir(buffer, sizeof(buffer));
    if (!length || length >= sizeof(buffer))
      return {};
    base = std::string(buffer) + "/dxmt/" + env::getExeName() + "/";
  }
  if (base.back() != '/')
    base += '/';
  std::string windows = "Z:";
  for (char c : base)
    windows += c == '/' ? '\\' : c;
  return windows;
}

PipelineRecords::PipelineRecords(WMTMetalVersion version) {
  if (env::getEnvVar("DXMT_PIPELINE_RECORDS") == "0")
    return;
  auto folder = cacheFolder();
  if (folder.empty()) {
    Logger::info("Pipeline records: no cache folder");
    return;
  }
  path_ = folder + str::format("pipelines_", (unsigned int)version, ".bin");
  enabled_ = true;
  load();
}

void
PipelineRecords::load() {
  FILE *file = fopen(path_.c_str(), "rb");
  if (!file)
    return;
  std::vector<uint8_t> bytes;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(file);
  if (bytes.size() < 8 || memcmp(bytes.data(), kMagic, 4) != 0)
    return;
  uint32_t file_version;
  memcpy(&file_version, bytes.data() + 4, 4);
  if (file_version != kPipelineRecordsVersion)
    return;
  size_t p = 8;
  while (p + 8 <= bytes.size()) {
    uint32_t kind, size;
    memcpy(&kind, bytes.data() + p, 4);
    memcpy(&size, bytes.data() + p + 4, 4);
    p += 8;
    if (size > bytes.size() - p)
      break; /* a truncated tail: the last write didn't finish */
    Record record{kind, std::vector<uint8_t>(bytes.begin() + p, bytes.begin() + p + size)};
    p += size;
    auto key = Sha1HashState::compute(record.data.data(), record.data.size()).string();
    if (seen_.insert(key).second)
      records_.push_back(std::move(record));
  }
  Logger::info(str::format("Pipeline records: ", records_.size(), " loaded"));
}

void
PipelineRecords::add(uint32_t kind, const void *data, uint32_t size) {
  if (!enabled_)
    return;
  auto key = Sha1HashState::compute(data, size).string();
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!seen_.insert(key).second)
    return;
  records_.push_back({kind, std::vector<uint8_t>((const uint8_t *)data, (const uint8_t *)data + size)});
  bool fresh = records_.size() == 1;
  FILE *file = fopen(path_.c_str(), fresh ? "wb" : "ab");
  if (!file) {
    /* could be a brand new file whose folder exists but which we can't append to yet */
    file = fopen(path_.c_str(), "wb");
    fresh = true;
    if (!file)
      return;
  }
  if (fresh) {
    fwrite(kMagic, 1, 4, file);
    fwrite(&kPipelineRecordsVersion, 4, 1, file);
  }
  fwrite(&kind, 4, 1, file);
  fwrite(&size, 4, 1, file);
  fwrite(data, 1, size, file);
  fclose(file);
}

void
PipelineRecords::addGraphics(CompiledShader *vs, CompiledShader *ps, const WMTRenderPipelineInfo &info) {
  if (!enabled_ || !vs)
    return;
  PipelineRecordGraphicsData data{};
  data.vs_sha1 = vs->ShaderDigest();
  data.vs_variant = vs->VariantDigest();
  if (ps) {
    data.has_ps = 1;
    data.ps_sha1 = ps->ShaderDigest();
    data.ps_variant = ps->VariantDigest();
  }
  data.info = info;
  data.info.vertex_function = NULL_OBJECT_HANDLE;
  data.info.fragment_function = NULL_OBJECT_HANDLE;
  data.info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  data.info.binary_archives_for_lookup.set(nullptr);
  data.info.num_binary_archives_for_lookup = 0;
  data.info.fail_on_binary_archive_miss = false;
  add(PipelineRecordGraphics, &data, sizeof(data));
}

void
PipelineRecords::addCompute(CompiledShader *cs, const WMTComputePipelineInfo &info) {
  if (!enabled_ || !cs)
    return;
  PipelineRecordComputeData data{};
  data.cs_sha1 = cs->ShaderDigest();
  data.cs_variant = cs->VariantDigest();
  data.info = info;
  data.info.compute_function = NULL_OBJECT_HANDLE;
  data.info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  data.info.binary_archives_for_lookup.set(nullptr);
  data.info.num_binary_archives_for_lookup = 0;
  data.info.fail_on_binary_archive_miss = false;
  add(PipelineRecordCompute, &data, sizeof(data));
}

std::vector<PipelineRecords::Record>
PipelineRecords::all() {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  return records_;
}

/* One recorded pipeline, built again: its functions come from the shader cache, so this costs only Metal's
 * build for this GPU, which Metal then remembers. Nothing is kept: the game's own pipeline objects are made
 * as usual, from Metal's cache. */
class WarmUpWork : public ThreadpoolWork {
public:
  WarmUpWork(MTLD3D11Device *device, ShaderCache &scache, PipelineRecords::Record record) :
      device_(device),
      scache_(scache),
      record_(std::move(record)) {}

  ThreadpoolWork *
  RunThreadpoolWork() final {
    auto pool = WMT::MakeAutoreleasePool();
    WMT::Reference<WMT::Error> err;
    if (record_.kind == PipelineRecordGraphics && record_.data.size() == sizeof(PipelineRecordGraphicsData)) {
      PipelineRecordGraphicsData data;
      memcpy(&data, record_.data.data(), sizeof(data));
      auto vs = function("vs_", data.vs_sha1, data.vs_variant);
      if (!vs)
        return this;
      WMT::Reference<WMT::Function> ps;
      if (data.has_ps) {
        ps = function("ps_", data.ps_sha1, data.ps_variant);
        if (!ps)
          return this;
      }
      WMTRenderPipelineInfo info = data.info;
      info.vertex_function = vs.handle;
      info.fragment_function = data.has_ps ? ps.handle : NULL_OBJECT_HANDLE;
      info.binary_archives_for_lookup.set(nullptr);
      info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
      auto state = device_->GetMTLDevice().newRenderPipelineState(info, err);
      if (!state)
        WARN("Warm-up: a recorded render pipeline failed: ", err.description().getUTF8String());
    } else if (record_.kind == PipelineRecordCompute && record_.data.size() == sizeof(PipelineRecordComputeData)) {
      PipelineRecordComputeData data;
      memcpy(&data, record_.data.data(), sizeof(data));
      auto cs = function("cs_", data.cs_sha1, data.cs_variant);
      if (!cs)
        return this;
      WMTComputePipelineInfo info = data.info;
      info.compute_function = cs.handle;
      info.binary_archives_for_lookup.set(nullptr);
      info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
      auto state = device_->GetMTLDevice().newComputePipelineState(info, err);
      if (!state)
        WARN("Warm-up: a recorded compute pipeline failed: ", err.description().getUTF8String());
    }
    return this;
  }

  bool GetIsDone() final { return done_; }
  void SetIsDone(bool state) final { done_.store(state); }

private:
  WMT::Reference<WMT::Function>
  function(const char *prefix, Sha1Digest &sha1, Sha1Digest &variant) {
    WMT::Reference<WMT::DispatchData> data;
    {
      auto reader = scache_.getReader();
      if (!reader)
        return {};
      data = reader->get(std::make_pair(sha1, variant));
    }
    if (!data)
      return {};
    WMT::Reference<WMT::Error> err;
    auto library = device_->GetMTLDevice().newLibrary(data, err);
    if (err || !library)
      return {};
    std::string name = std::string(prefix) + sha1.string().substr(0, 8) + "_" + variant.string();
    return library.newFunction(name.c_str());
  }

  MTLD3D11Device *device_;
  ShaderCache &scache_;
  PipelineRecords::Record record_;
  std::atomic_bool done_ = false;
};

void
WarmUpPipelines(MTLD3D11Device *device, ShaderCache &scache, std::vector<std::unique_ptr<ThreadpoolWork>> &works) {
  if (env::getEnvVar("DXMT_PIPELINE_WARMUP") == "0")
    return;
  if (!Config::getInstance().getOption<bool>("dxmt.warmUpPipelines", true))
    return;
  auto &records = PipelineRecords::getInstance(device->GetDXMTDevice().metalVersion());
  if (!records.enabled())
    return;
  auto all = records.all();
  if (all.empty())
    return;
  Logger::info(str::format("Pipeline warm-up: ", all.size(), " recorded pipelines"));
  works.reserve(all.size());
  for (auto &record : all)
    works.push_back(std::make_unique<WarmUpWork>(device, scache, std::move(record)));
}

} // namespace dxmt
