/* Added by the Sommelier project, 2026-10-22: the cellar tool (LGPL-2.1-or-later). */
/*
 * cellar: builds a game's recorded pipelines (DXMT's shaders_<metal>.db + pipelines_<metal>.bin) into a Metal
 * binary archive on this Mac, or checks an archive against them. The archive goes into dxmt/<exe>/cellar/ and
 * DXMT takes pipelines from it instead of compiling (see d3d11_cellar).
 *
 *   cellar cook  <shaders.db> <pipelines.bin> <out folder>   device-built archive `device.metallib`, plus
 *                                                            `libs/<key>.metallib` and `functions.tsv`
 *                                                            (function name, library file) for offline builds
 *   cellar check <shaders.db> <pipelines.bin> <archive>      how many recorded pipelines the archive holds
 *
 * Output is one line of key=value pairs on stdout; non-zero exit on failure.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <map>
#include <sqlite3.h>
#include <string>
#include <vector>
#include "winemetal.h"

namespace {

struct Sha1Digest { uint8_t data[20]; };
struct GraphicsRec { Sha1Digest vs_sha1, vs_variant, ps_sha1, ps_variant; uint32_t has_ps, padding; WMTRenderPipelineInfo info; };
struct ComputeRec { Sha1Digest cs_sha1, cs_variant; WMTComputePipelineInfo info; };

std::string hex(const uint8_t *d, size_t n) { std::string s(n * 2, '0'); for (size_t i = 0; i < n; i++) snprintf(&s[2 * i], 3, "%02x", d[i]); return s; }
double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Library { id<MTLLibrary> library; std::string key; NSData *data; };

/* The shader cache: key (shader digest + variant digest) → Metal library. The newest `cache_<n>` table. */
bool loadLibraries(id<MTLDevice> dev, const char *path, std::map<std::string, Library> &libs) {
  sqlite3 *db;
  std::string uri = std::string("file:") + path + "?immutable=1";
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) return false;
  sqlite3_stmt *st;
  std::string table;
  if (sqlite3_prepare_v2(db, "select name from sqlite_master where type='table' and name like 'cache_%' order by cast(substr(name,7) as integer) desc limit 1", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) table = (const char *)sqlite3_column_text(st, 0);
    sqlite3_finalize(st);
  }
  if (table.empty()) { sqlite3_close(db); return false; }
  std::string query = "select key, value from " + table;
  if (sqlite3_prepare_v2(db, query.c_str(), -1, &st, NULL) != SQLITE_OK) { sqlite3_close(db); return false; }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const void *k = sqlite3_column_blob(st, 0); int kl = sqlite3_column_bytes(st, 0);
    const void *v = sqlite3_column_blob(st, 1); int vl = sqlite3_column_bytes(st, 1);
    if (kl != 40) continue;
    NSData *data = [NSData dataWithBytes:v length:vl];
    dispatch_data_t dd = dispatch_data_create(v, vl, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&e];
    if (lib) libs[std::string((const char *)k, kl)] = {lib, hex((const uint8_t *)k, kl), data};
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return true;
}

struct Pipelines {
  std::vector<MTLRenderPipelineDescriptor *> render;
  std::vector<MTLComputePipelineDescriptor *> compute;
  int skipped = 0;
};

/* The records as Metal descriptors, the functions from the libraries (as d3d11_pipeline_records rebuilds them). */
Pipelines loadPipelines(const char *path, std::map<std::string, Library> &libs, std::vector<std::pair<std::string, std::string>> *functions) {
  Pipelines out;
  NSData *bin = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
  if (!bin || bin.length < 8) return out;
  auto fn = [&](const char *prefix, const Sha1Digest &s, const Sha1Digest &v) -> id<MTLFunction> {
    std::string key = std::string((const char *)s.data, 20) + std::string((const char *)v.data, 20);
    auto it = libs.find(key);
    if (it == libs.end()) return nil;
    std::string name = std::string(prefix) + hex(s.data, 20).substr(0, 8) + "_" + hex(v.data, 20);
    if (functions) functions->push_back({name, it->second.key});
    return [it->second.library newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]];
  };
  const uint8_t *b = (const uint8_t *)bin.bytes;
  size_t p = 8, n = bin.length;
  while (p + 8 <= n) {
    uint32_t kind, size;
    memcpy(&kind, b + p, 4); memcpy(&size, b + p + 4, 4);
    p += 8;
    if (size > n - p) break;
    const uint8_t *data = b + p;
    p += size;
    if (kind == 1 && size == sizeof(GraphicsRec)) {
      GraphicsRec r; memcpy(&r, data, sizeof(r));
      id<MTLFunction> vs = fn("vs_", r.vs_sha1, r.vs_variant);
      id<MTLFunction> ps = r.has_ps ? fn("ps_", r.ps_sha1, r.ps_variant) : nil;
      if (!vs || (r.has_ps && !ps)) { out.skipped++; continue; }
      MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
      for (unsigned i = 0; i < 8; i++) {
        auto &c = r.info.colors[i];
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)ORIGINAL_FORMAT(c.pixel_format);
        d.colorAttachments[i].blendingEnabled = c.blending_enabled;
        d.colorAttachments[i].writeMask = c.write_mask;
        d.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)c.alpha_blend_operation;
        d.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)c.rgb_blend_operation;
        d.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)c.src_rgb_blend_factor;
        d.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)c.src_alpha_blend_factor;
        d.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)c.dst_rgb_blend_factor;
        d.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)c.dst_alpha_blend_factor;
      }
      for (unsigned i = 0; i < 31; i++) {
        if (r.info.immutable_fragment_buffers & (1u << i)) d.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
        if (r.info.immutable_vertex_buffers & (1u << i)) d.vertexBuffers[i].mutability = MTLMutabilityImmutable;
      }
      d.depthAttachmentPixelFormat = (MTLPixelFormat)ORIGINAL_FORMAT(r.info.depth_pixel_format);
      d.stencilAttachmentPixelFormat = (MTLPixelFormat)ORIGINAL_FORMAT(r.info.stencil_pixel_format);
      d.alphaToCoverageEnabled = r.info.alpha_to_coverage_enabled;
      d.rasterizationEnabled = r.info.rasterization_enabled;
      d.rasterSampleCount = r.info.raster_sample_count;
      d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)r.info.input_primitive_topology;
      d.tessellationPartitionMode = (MTLTessellationPartitionMode)r.info.tessellation_partition_mode;
      d.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)r.info.tessellation_factor_step;
      d.tessellationOutputWindingOrder = (MTLWinding)r.info.tessellation_output_winding_order;
      d.maxTessellationFactor = r.info.max_tessellation_factor;
      d.vertexFunction = vs;
      d.fragmentFunction = ps;
      out.render.push_back(d);
    } else if (kind == 2 && size == sizeof(ComputeRec)) {
      ComputeRec r; memcpy(&r, data, sizeof(r));
      id<MTLFunction> cs = fn("cs_", r.cs_sha1, r.cs_variant);
      if (!cs) { out.skipped++; continue; }
      MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
      d.computeFunction = cs;
      d.threadGroupSizeIsMultipleOfThreadExecutionWidth = r.info.tgsize_is_multiple_of_sgwidth;
      for (unsigned i = 0; i < 31; i++)
        if (r.info.immutable_buffers & (1u << i)) d.buffers[i].mutability = MTLMutabilityImmutable;
      out.compute.push_back(d);
    } else {
      out.skipped++;
    }
  }
  return out;
}

int cook(id<MTLDevice> dev, const char *shaders, const char *records, const char *folder) {
  std::map<std::string, Library> libs;
  if (!loadLibraries(dev, shaders, libs)) { fprintf(stderr, "cellar: can't read %s\n", shaders); return 2; }
  std::vector<std::pair<std::string, std::string>> functions;
  auto pipelines = loadPipelines(records, libs, &functions);
  NSString *out = [NSString stringWithUTF8String:folder];
  NSFileManager *fm = [NSFileManager defaultManager];
  [fm createDirectoryAtPath:[out stringByAppendingPathComponent:@"libs"] withIntermediateDirectories:YES attributes:nil error:nil];
  NSError *err = nil;
  id<MTLBinaryArchive> archive = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&err];
  if (!archive) { fprintf(stderr, "cellar: %s\n", err.localizedDescription.UTF8String); return 2; }
  double started = now();
  struct Progress { std::atomic<int> built{0}, failed{0}; std::mutex archive; };
  auto progress = std::make_shared<Progress>();
  /* Built on every core; one archive write at a time. */
  dispatch_apply(pipelines.render.size() + pipelines.compute.size(), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) {
    @autoreleasepool {
      NSError *e = nil;
      id state = nil;
      if (i < pipelines.render.size()) state = [dev newRenderPipelineStateWithDescriptor:pipelines.render[i] options:MTLPipelineOptionNone reflection:nil error:&e];
      else state = [dev newComputePipelineStateWithDescriptor:pipelines.compute[i - pipelines.render.size()] options:MTLPipelineOptionNone reflection:nil error:&e];
      if (!state) { progress->failed++; return; }
      std::lock_guard<std::mutex> lock(progress->archive);
      BOOL ok = i < pipelines.render.size() ? [archive addRenderPipelineFunctionsWithDescriptor:pipelines.render[i] error:&e]
                                            : [archive addComputePipelineFunctionsWithDescriptor:pipelines.compute[i - pipelines.render.size()] error:&e];
      if (ok) progress->built++; else progress->failed++;
    }
  });
  int built = progress->built.load(), failed = progress->failed.load();
  double ms = now() - started;
  NSString *archivePath = [out stringByAppendingPathComponent:@"device.metallib"];
  if (built > 0 && ![archive serializeToURL:[NSURL fileURLWithPath:archivePath] error:&err]) {
    fprintf(stderr, "cellar: %s\n", err.localizedDescription.UTF8String);
    return 2;
  }
  /* The libraries and the function → library table, for offline builds with metal-tt. */
  for (auto &[key, lib] : libs)
    [lib.data writeToFile:[[out stringByAppendingPathComponent:@"libs"] stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.metallib", lib.key.c_str()]] atomically:YES];
  NSMutableString *table = [NSMutableString new];
  for (auto &[name, key] : functions) [table appendFormat:@"%s\tlibs/%s.metallib\n", name.c_str(), key.c_str()];
  [table writeToFile:[out stringByAppendingPathComponent:@"functions.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
  unsigned long long size = built > 0 ? [[fm attributesOfItemAtPath:archivePath error:nil] fileSize] : 0;
  printf("built=%d failed=%d skipped=%d libraries=%zu ms=%.0f bytes=%llu device=%s\n", built, failed, pipelines.skipped, libs.size(), ms, size, dev.name.UTF8String);
  return failed > 0 && built == 0 ? 1 : 0;
}

int check(id<MTLDevice> dev, const char *shaders, const char *records, const char *archivePath) {
  std::map<std::string, Library> libs;
  if (!loadLibraries(dev, shaders, libs)) { fprintf(stderr, "cellar: can't read %s\n", shaders); return 2; }
  auto pipelines = loadPipelines(records, libs, nullptr);
  MTLBinaryArchiveDescriptor *ad = [MTLBinaryArchiveDescriptor new];
  ad.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archivePath]];
  NSError *err = nil;
  id<MTLBinaryArchive> archive = [dev newBinaryArchiveWithDescriptor:ad error:&err];
  if (!archive) { fprintf(stderr, "cellar: %s\n", err.localizedDescription.UTF8String); return 2; }
  int hit = 0, miss = 0;
  double started = now();
  for (auto d : pipelines.render) {
    @autoreleasepool {
      d.binaryArchives = @[archive];
      NSError *e = nil;
      if ([dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e]) hit++; else miss++;
    }
  }
  for (auto d : pipelines.compute) {
    @autoreleasepool {
      d.binaryArchives = @[archive];
      NSError *e = nil;
      if ([dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e]) hit++; else miss++;
    }
  }
  printf("hit=%d miss=%d skipped=%d ms=%.0f device=%s\n", hit, miss, pipelines.skipped, now() - started, dev.name.UTF8String);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 5) {
      fprintf(stderr, "usage: cellar cook <shaders.db> <pipelines.bin> <out folder>\n       cellar check <shaders.db> <pipelines.bin> <archive>\n");
      return 2;
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "cellar: no Metal device\n"); return 2; }
    std::string mode = argv[1];
    if (mode == "cook") return cook(dev, argv[2], argv[3], argv[4]);
    if (mode == "check") return check(dev, argv[2], argv[3], argv[4]);
    fprintf(stderr, "cellar: unknown mode %s\n", argv[1]);
    return 2;
  }
}
