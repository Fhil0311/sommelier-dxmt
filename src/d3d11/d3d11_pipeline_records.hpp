/* Added by the Sommelier project, 2026-10-20: pipeline records and warm-up (LGPL-2.1-or-later). */
#pragma once
/*
 * Pipeline records: what a game asked Metal to build, kept so the next run can build it all before the game
 * draws. A record names each stage's shader by its shader-cache keys (shader digest, variant digest) and keeps
 * the rest of the pipeline description as Metal receives it. The shader translations themselves are in the
 * shader cache; records are the same on every Mac, so they can travel with it.
 *
 * File: `dxmt/<exe>/pipelines_<metal>.bin` under the user's cache folder (or DXMT_SHADER_CACHE_PATH):
 * "DXPL", version (u32), then records of [kind (u32), size (u32), data]. Little endian, append-only.
 *
 * DXMT_PIPELINE_RECORDS=0 turns recording off; DXMT_PIPELINE_WARMUP=0 turns the warm-up off.
 */
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "dxmt_shader_cache.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include "winemetal.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace dxmt {

constexpr uint32_t kPipelineRecordsVersion = 1;

enum PipelineRecordKind : uint32_t {
  PipelineRecordGraphics = 1,
  PipelineRecordCompute = 2,
};

struct PipelineRecordGraphicsData {
  Sha1Digest vs_sha1;
  Sha1Digest vs_variant;
  Sha1Digest ps_sha1;
  Sha1Digest ps_variant;
  uint32_t has_ps;
  uint32_t padding;
  WMTRenderPipelineInfo info; /* function handles and binary archive fields zeroed */
};

struct PipelineRecordComputeData {
  Sha1Digest cs_sha1;
  Sha1Digest cs_variant;
  WMTComputePipelineInfo info; /* likewise */
};

class PipelineRecords {
public:
  static PipelineRecords &getInstance(WMTMetalVersion version);

  PipelineRecords(WMTMetalVersion version);
  PipelineRecords(const PipelineRecords &) = delete;

  void addGraphics(CompiledShader *vs, CompiledShader *ps, const WMTRenderPipelineInfo &info);
  void addCompute(CompiledShader *cs, const WMTComputePipelineInfo &info);

  struct Record {
    uint32_t kind;
    std::vector<uint8_t> data;
  };
  /* Everything recorded so far, this run included. */
  std::vector<Record> all();

  bool enabled() const { return enabled_; }

private:
  void add(uint32_t kind, const void *data, uint32_t size);
  void load();

  std::string path_;
  bool enabled_ = false;
  bool warned_ = false;
  dxmt::mutex mutex_;
  std::unordered_set<std::string> seen_;
  std::vector<Record> records_;
};

/* Builds every recorded pipeline whose shaders the shader cache has, as thread pool works to submit. */
void WarmUpPipelines(MTLD3D11Device *device, ShaderCache &scache, std::vector<std::unique_ptr<ThreadpoolWork>> &works);

} // namespace dxmt
