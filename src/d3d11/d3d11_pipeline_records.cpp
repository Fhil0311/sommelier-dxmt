/* Added by the Sommelier project, 2026-10-20: pipeline records and warm-up (LGPL-2.1-or-later). */
#include "d3d11_pipeline_records.hpp"
#include "d3d11_cellar.hpp"
#include "Metal.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <chrono>
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

std::string
PipelineCacheFolderUnix() {
  std::string base = env::getEnvVar("DXMT_SHADER_CACHE_PATH");
  if (base.empty() || base[0] != '/') {
    char buffer[4096];
    uint64_t length = WMTGetUserCacheDir(buffer, sizeof(buffer));
    if (!length || length >= sizeof(buffer))
      return {};
    base = std::string(buffer);
    if (base.back() != '/')
      base += '/';
    base += "dxmt/" + env::getExeName() + "/";
  }
  if (base.back() != '/')
    base += '/';
  return base;
}

/* The folder DXMT's caches live in, as a path this (Windows) side can open: Wine shows the unix tree as Z:. */
static std::string
cacheFolder() {
  std::string base = PipelineCacheFolderUnix();
  if (base.empty())
    return {};
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
    if (!file) {
      if (!warned_) {
        warned_ = true;
        Logger::warn(str::format("Pipeline records: can't write ", path_));
      }
      return;
    }
  }
  if (records_.size() == 1)
    Logger::info(str::format("Pipeline records: writing to ", path_));
  if (fresh) {
    fwrite(kMagic, 1, 4, file);
    fwrite(&kPipelineRecordsVersion, 4, 1, file);
  }
  fwrite(&kind, 4, 1, file);
  fwrite(&size, 4, 1, file);
  fwrite(data, 1, size, file);
  fclose(file);
}

/* The same pipeline must give the same bytes on every run, so the records dedupe: the structs are copied field
 * by field into zeroed memory (a struct copy would carry their padding bytes, which hold whatever was on the
 * stack), with the per-process fields (functions, archives) left zero. */
static void
normalize(const WMTRenderPipelineInfo &in, WMTRenderPipelineInfo &out) {
  memset(&out, 0, sizeof(out));
  for (unsigned i = 0; i < 8; i++) {
    out.colors[i].pixel_format = in.colors[i].pixel_format;
    out.colors[i].rgb_blend_operation = in.colors[i].rgb_blend_operation;
    out.colors[i].alpha_blend_operation = in.colors[i].alpha_blend_operation;
    out.colors[i].src_rgb_blend_factor = in.colors[i].src_rgb_blend_factor;
    out.colors[i].dst_rgb_blend_factor = in.colors[i].dst_rgb_blend_factor;
    out.colors[i].src_alpha_blend_factor = in.colors[i].src_alpha_blend_factor;
    out.colors[i].dst_alpha_blend_factor = in.colors[i].dst_alpha_blend_factor;
    out.colors[i].write_mask = in.colors[i].write_mask;
    out.colors[i].blending_enabled = in.colors[i].blending_enabled;
  }
  out.alpha_to_coverage_enabled = in.alpha_to_coverage_enabled;
  out.logic_operation_enabled = in.logic_operation_enabled;
  out.logic_operation = in.logic_operation;
  out.rasterization_enabled = in.rasterization_enabled;
  out.raster_sample_count = in.raster_sample_count;
  out.depth_pixel_format = in.depth_pixel_format;
  out.stencil_pixel_format = in.stencil_pixel_format;
  out.immutable_vertex_buffers = in.immutable_vertex_buffers;
  out.immutable_fragment_buffers = in.immutable_fragment_buffers;
  out.input_primitive_topology = in.input_primitive_topology;
  out.tessellation_partition_mode = in.tessellation_partition_mode;
  out.max_tessellation_factor = in.max_tessellation_factor;
  out.tessellation_output_winding_order = in.tessellation_output_winding_order;
  out.tessellation_factor_step = in.tessellation_factor_step;
}

static void
normalize(const WMTComputePipelineInfo &in, WMTComputePipelineInfo &out) {
  memset(&out, 0, sizeof(out));
  out.tgsize_is_multiple_of_sgwidth = in.tgsize_is_multiple_of_sgwidth;
  out.immutable_buffers = in.immutable_buffers;
}

void
PipelineRecords::addGraphics(CompiledShader *vs, CompiledShader *ps, const WMTRenderPipelineInfo &info) {
  if (!enabled_ || !vs)
    return;
  PipelineRecordGraphicsData data;
  memset(&data, 0, sizeof(data));
  data.vs_sha1 = vs->ShaderDigest();
  data.vs_variant = vs->VariantDigest();
  if (ps) {
    data.has_ps = 1;
    data.ps_sha1 = ps->ShaderDigest();
    data.ps_variant = ps->VariantDigest();
  }
  normalize(info, data.info);
  add(PipelineRecordGraphics, &data, sizeof(data));
}

void
PipelineRecords::addCompute(CompiledShader *cs, const WMTComputePipelineInfo &info) {
  if (!enabled_ || !cs)
    return;
  PipelineRecordComputeData data;
  memset(&data, 0, sizeof(data));
  data.cs_sha1 = cs->ShaderDigest();
  data.cs_variant = cs->VariantDigest();
  normalize(info, data.info);
  add(PipelineRecordCompute, &data, sizeof(data));
}

std::vector<PipelineRecords::Record>
PipelineRecords::all() {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  return records_;
}

/* What one warm-up amounted to, logged by the last work to finish. */
struct WarmUpSummary {
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::atomic_uint32_t total = 0;
  std::atomic_uint32_t finished = 0;
  std::atomic_uint32_t built = 0;
  /* Built in under 2 ms: taken from the cellar (or Metal's cache) rather than compiled. */
  std::atomic_uint32_t instant = 0;
  std::atomic_uint32_t skipped = 0;
  std::atomic_uint32_t failed = 0;

  void
  done(bool was_built, bool was_skipped, double ms) {
    if (was_built) {
      built++;
      if (ms < 2.0)
        instant++;
    } else if (was_skipped)
      skipped++;
    else
      failed++;
    if (++finished == total.load()) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      Logger::info(str::format("Pipeline warm-up: ", built.load(), " built (", instant.load(), " ready-made), ", skipped.load(),
                               " without cached shaders, ", failed.load(), " failed, in ", ms, " ms"));
    }
  }
};

/* One recorded pipeline, built again: its functions come from the shader cache, so this costs only Metal's
 * build for this GPU, which Metal then remembers. Nothing is kept: the game's own pipeline objects are made
 * as usual, from Metal's cache. */
class WarmUpWork : public ThreadpoolWork {
public:
  WarmUpWork(MTLD3D11Device *device, ShaderCache &scache, PipelineRecords::Record record, std::shared_ptr<WarmUpSummary> summary) :
      device_(device),
      scache_(scache),
      record_(std::move(record)),
      summary_(std::move(summary)) {}

  ThreadpoolWork *
  RunThreadpoolWork() final {
    bool built = false, skipped = false;
    auto started = std::chrono::steady_clock::now();
    run(built, skipped);
    summary_->done(built, skipped, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return this;
  }

  void
  run(bool &built, bool &skipped) {
    auto pool = WMT::MakeAutoreleasePool();
    WMT::Reference<WMT::Error> err;
    if (record_.kind == PipelineRecordGraphics && record_.data.size() == sizeof(PipelineRecordGraphicsData)) {
      PipelineRecordGraphicsData data;
      memcpy(&data, record_.data.data(), sizeof(data));
      auto vs = function("vs_", data.vs_sha1, data.vs_variant);
      if (!vs) {
        skipped = true;
        return;
      }
      WMT::Reference<WMT::Function> ps;
      if (data.has_ps) {
        ps = function("ps_", data.ps_sha1, data.ps_variant);
        if (!ps) {
          skipped = true;
          return;
        }
      }
      WMTRenderPipelineInfo info = data.info;
      info.vertex_function = vs.handle;
      info.fragment_function = data.has_ps ? ps.handle : NULL_OBJECT_HANDLE;
      info.binary_archives_for_lookup.set(nullptr);
      info.num_binary_archives_for_lookup = 0;
      info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
      Cellar::getInstance(device_->GetDXMTDevice().metalVersion()).lookup(info);
      auto state = device_->GetMTLDevice().newRenderPipelineState(info, err);
      if (!state)
        WARN("Warm-up: a recorded render pipeline failed: ", err.description().getUTF8String());
      built = state != nullptr;
    } else if (record_.kind == PipelineRecordCompute && record_.data.size() == sizeof(PipelineRecordComputeData)) {
      PipelineRecordComputeData data;
      memcpy(&data, record_.data.data(), sizeof(data));
      auto cs = function("cs_", data.cs_sha1, data.cs_variant);
      if (!cs) {
        skipped = true;
        return;
      }
      WMTComputePipelineInfo info = data.info;
      info.compute_function = cs.handle;
      info.binary_archives_for_lookup.set(nullptr);
      info.num_binary_archives_for_lookup = 0;
      info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
      Cellar::getInstance(device_->GetDXMTDevice().metalVersion()).lookup(info);
      auto state = device_->GetMTLDevice().newComputePipelineState(info, err);
      if (!state)
        WARN("Warm-up: a recorded compute pipeline failed: ", err.description().getUTF8String());
      built = state != nullptr;
    }
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
  std::shared_ptr<WarmUpSummary> summary_;
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
  auto summary = std::make_shared<WarmUpSummary>();
  summary->total = all.size();
  works.reserve(all.size());
  for (auto &record : all)
    works.push_back(std::make_unique<WarmUpWork>(device, scache, std::move(record), summary));
}

} // namespace dxmt
