/*
 * LightweightVK
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Bistro.h"
#include "VulkanApp.h"

// scene navigation
#define USE_SPONZA 0

#if USE_SPONZA
#define MODEL_PATH "src/Sponza/sponza.obj"
#define CACHE_FILE_NAME "cache3.data"
vec3 lightDir_ = normalize(vec3(0.05f, 1.0f, 0.01f));
#else
#define MODEL_PATH "src/bistro/Exterior/exterior.obj"
#define CACHE_FILE_NAME "cache2.data"
vec3 lightDir_ = normalize(vec3(0.032f, 0.835f, 0.549f));
#endif

#define UBOS_AND_PUSH_CONSTANTS \
  R"(
struct Material {
  vec4 ambient;
  vec4 diffuse;
};

layout(std430, buffer_reference) readonly buffer Materials {
  Material mtl[];
};

struct Vertex {
  float x, y, z;
  uint uv;
  uint16_t normal; // Octahedral 16-bit https://www.shadertoy.com/view/llfcRl
  uint16_t mtlIndex;
};

layout(std430, buffer_reference) readonly buffer Vertices {
  Vertex vtx[];
};

layout(std430, buffer_reference) readonly buffer Indices {
  uint idx[];
};

layout(std430, buffer_reference) readonly buffer PerFrame {
  mat4 viewInverse;
  mat4 projInverse;
};

layout(push_constant) uniform constants {
  vec4 lightDir;
  PerFrame perFrame;
  Materials materials;
  Indices indices;
  Vertices vertices;
  uint outTexture;
  uint tlas;
};
)"

const char* codeRayGen = R"(
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_16bit_storage : require

layout (set = 0, binding = 2, rgba8) uniform image2D kTextures2DInOut[];
layout (set = 0, binding = 4) uniform accelerationStructureEXT kTLAS[];
)" UBOS_AND_PUSH_CONSTANTS
                         R"(
layout(location = 0) rayPayloadEXT vec4 payload;

const float tmin = 0.001;
const float tmax = 500.0;

void main() {
  vec2 pixelCenter = gl_LaunchIDEXT.xy + vec2(0.5);
  vec2 d = 2.0 * (pixelCenter / gl_LaunchSizeEXT.xy) - 1.0;

  vec4 origin = perFrame.viewInverse * vec4(0,0,0,1);
  vec4 target = perFrame.projInverse * vec4(d, 1, 1);
  vec4 direction = perFrame.viewInverse * vec4(normalize(target.xyz), 0);

  payload = vec4(0.0, 0.0, 0.0, 1.0);

  traceRayEXT(kTLAS[tlas], gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0, origin.xyz, tmin, direction.xyz, tmax, 0);

  imageStore(kTextures2DInOut[outTexture], ivec2(gl_LaunchIDEXT.xy), payload);
}
)";

const char* codeMiss = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
  payload = vec4(1.0, 1.0, 1.0, 1.0);
})";

const char* codeMissShadow = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT bool isShadowed;

void main() {
  isShadowed = false;
})";

const char* codeClosestHit = R"(
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_16bit_storage : require

const float tmin = 0.001;
const float tmax = 500.0;

layout (set = 0, binding = 4) uniform accelerationStructureEXT kTLAS[];

layout(location = 0) rayPayloadInEXT vec4 payload;
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec2 attribs;
)" UBOS_AND_PUSH_CONSTANTS
                             R"(

// https://www.shadertoy.com/view/llfcRl
vec2 unpackSnorm2x8(uint d) {
  return vec2(uvec2(d, d >> 8) & 255u) / 127.5 - 1.0;
}
vec3 unpackOctahedral16(uint data) {
  vec2 v = unpackSnorm2x8(data);
  // https://x.com/Stubbesaurus/status/937994790553227264
  vec3 n = vec3(v, 1.0 - abs(v.x) - abs(v.y));
  float t = max(-n.z, 0.0);
  n.x += (n.x > 0.0) ? -t : t;
  n.y += (n.y > 0.0) ? -t : t;
  return normalize(n);
}
//

void main() {
  const vec3 baryCoords = vec3(1.0f - attribs.x - attribs.y, attribs.x, attribs.y);

  uint index = 3 * gl_PrimitiveID;
  ivec3 triangleIndex = ivec3(indices.idx[index + 0], indices.idx[index + 1], indices.idx[index + 2]);

  vec3 nrm0 = unpackOctahedral16(uint(vertices.vtx[triangleIndex.x].normal));
  vec3 nrm1 = unpackOctahedral16(uint(vertices.vtx[triangleIndex.y].normal));
  vec3 nrm2 = unpackOctahedral16(uint(vertices.vtx[triangleIndex.z].normal));
  vec3 normal = normalize(nrm0 * baryCoords.x + nrm1 * baryCoords.y + nrm2 * baryCoords.z);
  vec3 worldNormal = normalize(vec3(normal * gl_WorldToObjectEXT));

  Material mat = materials.mtl[uint(vertices.vtx[triangleIndex.x].mtlIndex)];

  float shadow = 1.0;
  if (dot(lightDir.xyz, worldNormal) > 0) {
    isShadowed = true;
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    traceRayEXT(kTLAS[tlas], gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xff, 0, 0, 1, hitPoint, tmin, lightDir.xyz, tmax, 1);
    if (isShadowed) {
      shadow = 0.6;
    }
  }

  float NdotL1 = clamp(dot(worldNormal, normalize(vec3(-1, 1,+1))), 0.0, 1.0);
  float NdotL2 = clamp(dot(worldNormal, normalize(vec3(-1, 1,-1))), 0.0, 1.0);
  float NdotL = 0.5 * (NdotL1 + NdotL2);

  payload = vec4(mat.diffuse.rgb * shadow * max(NdotL, 0.0), mat.diffuse.a);
}
)";

const char* kCodeFullscreenVS = R"(
layout (location=0) out vec2 uv;
void main() {
  // generate a triangle covering the entire screen
  uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(uv * vec2(2, -2) + vec2(-1, 1), 0.0, 1.0);
}
)";

const char* kCodeFullscreenFS = R"(
layout (location=0) in vec2 uv;
layout (location=0) out vec4 out_FragColor;

layout(push_constant) uniform constants {
	uint tex;
} pc;

void main() {
  out_FragColor = textureBindless2D(pc.tex, 0, vec2(uv.x, 1.0 - uv.y));
}
)";

lvk::IContext* ctx_ = nullptr;

struct {
  lvk::Holder<lvk::ShaderModuleHandle> smFullscreenVert_;
  lvk::Holder<lvk::ShaderModuleHandle> smFullscreenFrag_;
  lvk::Holder<lvk::ShaderModuleHandle> smRaygen_;
  lvk::Holder<lvk::ShaderModuleHandle> smMiss_;
  lvk::Holder<lvk::ShaderModuleHandle> smMissShadow_;
  lvk::Holder<lvk::ShaderModuleHandle> smHit_;
  lvk::Holder<lvk::RenderPipelineHandle> renderPipelineState_Fullscreen_;
  lvk::Holder<lvk::BufferHandle> vb0_, ib0_; // buffers for vertices and indices
  lvk::Holder<lvk::BufferHandle> sbMaterials_; // storage buffer for materials
  lvk::Holder<lvk::BufferHandle> sbInstances_; // storage buffer for TLAS instances
  lvk::Holder<lvk::BufferHandle> ubPerFrame_;
  std::vector<lvk::Holder<lvk::AccelStructHandle>> BLAS_;
  lvk::Holder<lvk::AccelStructHandle> TLAS_;
  lvk::Holder<lvk::TextureHandle> rayTracingOutputImage_;
  lvk::Holder<lvk::RayTracingPipelineHandle> rayTracingPipeline_;
} res;

struct UniformsPerFrame {
  mat4 viewInverse;
  mat4 projInverse;
} perFrame_;

// this goes into our GLSL shaders
struct GPUMaterial {
  vec4 ambient = vec4(0.0f);
  vec4 diffuse = vec4(0.0f);
};

static_assert(sizeof(GPUMaterial) % 16 == 0);

std::vector<GPUMaterial> materials_;

void createPipelines();

bool initModel(const std::string& folderContentRoot) {
  const std::string cacheFileName = folderContentRoot + CACHE_FILE_NAME;

  if (!loadFromCache(cacheFileName.c_str())) {
    if (!LVK_VERIFY(loadAndCache(folderContentRoot, cacheFileName.c_str(), MODEL_PATH))) {
      LVK_ASSERT_MSG(false, "Cannot load 3D model");
      return false;
    }
  }

  for (const auto& mtl : cachedMaterials_) {
    materials_.push_back(GPUMaterial{vec4(mtl.ambient, 1.0f), vec4(mtl.diffuse, 1.0f)});
  }
  res.sbMaterials_ = ctx_->createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                         .storage = lvk::StorageType_Device,
                                         .size = sizeof(GPUMaterial) * materials_.size(),
                                         .data = materials_.data(),
                                         .debugName = "Buffer: materials"},
                                        nullptr);

  res.vb0_ = ctx_->createBuffer({.usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
                                 .storage = lvk::StorageType_Device,
                                 .size = sizeof(VertexData) * vertexData_.size(),
                                 .data = vertexData_.data(),
                                 .debugName = "Buffer: vertex"},
                                nullptr);
  res.ib0_ = ctx_->createBuffer({.usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
                                 .storage = lvk::StorageType_Device,
                                 .size = sizeof(uint32_t) * indexData_.size(),
                                 .data = indexData_.data(),
                                 .debugName = "Buffer: index"},
                                nullptr);

  const glm::mat3x4 transformMatrix(1.0f);

  lvk::Holder<lvk::BufferHandle> transformBuffer = ctx_->createBuffer({
      .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
      .storage = lvk::StorageType_HostVisible,
      .size = sizeof(glm::mat3x4),
      .data = &transformMatrix,
  });

  const auto totalPrimitiveCount = (uint32_t)indexData_.size() / 3;
  lvk::AccelStructDesc blasDesc{
      .type = lvk::AccelStructType_BLAS,
      .geometryType = lvk::AccelStructGeomType_Triangles,
      .vertexFormat = lvk::VertexFormat::Float3,
      .vertexBuffer = res.vb0_,
      .vertexStride = sizeof(VertexData),
      .numVertices = (uint32_t)vertexData_.size(),
      .indexFormat = lvk::IndexFormat_UI32,
      .indexBuffer = res.ib0_,
      .transformBuffer = transformBuffer,
      .buildRange = {.primitiveCount = totalPrimitiveCount},
      .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastTrace,
      .debugName = "BLAS",
  };
  const lvk::AccelStructSizes blasSizes = ctx_->getAccelStructSizes(blasDesc);
  LLOGL("Full model BLAS sizes (byts):\n   buildScratchSize = %llu,\n   accelerationStructureSize = %llu\n",
        blasSizes.buildScratchSize,
        blasSizes.accelerationStructureSize);
  const uint32_t maxStorageBufferSize = ctx_->getMaxStorageBufferRange();

  // Calculate number of BLAS
  const uint32_t requiredBlasCount = [&blasSizes, maxStorageBufferSize]() {
    const uint32_t count1 = blasSizes.buildScratchSize / maxStorageBufferSize;
    const uint32_t count2 = blasSizes.accelerationStructureSize / maxStorageBufferSize;
    return 1 + (count1 > count2 ? count1 : count2);
  }();
  blasDesc.buildRange.primitiveCount = totalPrimitiveCount / requiredBlasCount;

  LVK_ASSERT(requiredBlasCount > 0);
  LLOGL("maxStorageBufferSize = %u bytes\nNumber of BLAS = %u\n", maxStorageBufferSize, requiredBlasCount);

  const glm::mat3x4 transform(glm::scale(mat4(1.0f), vec3(0.05f)));
  res.BLAS_.reserve(requiredBlasCount);

  std::vector<lvk::AccelStructInstance> instances;
  instances.reserve(requiredBlasCount);
  const auto primitiveCount = blasDesc.buildRange.primitiveCount;
  for (int i = 0; i < totalPrimitiveCount; i += (int)primitiveCount) {
    const auto rest = (int)totalPrimitiveCount - i;
    blasDesc.buildRange.primitiveOffset = (uint32_t)i * 3 * sizeof(uint32_t);
    blasDesc.buildRange.primitiveCount = (primitiveCount < rest) ? primitiveCount : rest;
    res.BLAS_.emplace_back(ctx_->createAccelerationStructure(blasDesc));
    instances.emplace_back(lvk::AccelStructInstance{
        // clang-format off
        .transform = (const lvk::mat3x4&)transform,
        // clang-format on
        .instanceCustomIndex = 0,
        .mask = 0xff,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = lvk::AccelStructInstanceFlagBits_TriangleFacingCullDisable,
        .accelerationStructureReference = ctx_->gpuAddress(res.BLAS_.back()),
    });
  }

  // Buffer for instance data
  res.sbInstances_ = ctx_->createBuffer(lvk::BufferDesc{
      .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
      .storage = lvk::StorageType_HostVisible,
      .size = sizeof(lvk::AccelStructInstance) * instances.size(),
      .data = instances.data(),
      .debugName = "sbInstances_",
  });

  res.TLAS_ = ctx_->createAccelerationStructure({
      .type = lvk::AccelStructType_TLAS,
      .geometryType = lvk::AccelStructGeomType_Instances,
      .instancesBuffer = res.sbInstances_,
      .buildRange = {.primitiveCount = (uint32_t)instances.size()},
      .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastTrace,
  });

  return true;
}

VULKAN_APP_MAIN {
  const VulkanAppConfig cfg{
      .width = 0,
      .height = 0,
#if USE_SPONZA
      .initialCameraPos = vec3(-25, 10, -1),
      .initialCameraTarget = vec3(10, 10, 0),
#else
      .initialCameraPos = vec3(-100, 40, -47),
      .initialCameraTarget = vec3(0, 35, 0),
#endif
  };
  VULKAN_APP_DECLARE(app, cfg);

  ctx_ = app.ctx_.get();

  res.ubPerFrame_ = ctx_->createBuffer({
      .usage = lvk::BufferUsageBits_Storage,
      .storage = lvk::StorageType_HostVisible,
      .size = sizeof(UniformsPerFrame),
      .debugName = "Buffer: uniforms (per frame)",
  });

  res.rayTracingOutputImage_ = ctx_->createTexture(lvk::TextureDesc{
      .type = lvk::TextureType_2D,
      .format = lvk::Format_BGRA_UN8,
      .dimensions = {(uint32_t)app.width_, (uint32_t)app.height_},
      .usage = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .debugName = "Ray-Tracing Output Image",
  });

  res.smRaygen_ = ctx_->createShaderModule({codeRayGen, lvk::Stage_RayGen, "Shader Module: main (raygen)"});
  res.smMiss_ = ctx_->createShaderModule({codeMiss, lvk::Stage_Miss, "Shader Module: main (miss)"});
  res.smMissShadow_ = ctx_->createShaderModule({codeMissShadow, lvk::Stage_Miss, "Shader Module: main (miss shadow)"});
  res.smHit_ = ctx_->createShaderModule({codeClosestHit, lvk::Stage_ClosestHit, "Shader Module: main (closesthit)"});

  res.rayTracingPipeline_ = ctx_->createRayTracingPipeline(lvk::RayTracingPipelineDesc{
      .smRayGen = {lvk::ShaderModuleHandle(res.smRaygen_)},
      .smClosestHit = {lvk::ShaderModuleHandle(res.smHit_)},
      .smMiss =
          {
              lvk::ShaderModuleHandle(res.smMiss_),
              lvk::ShaderModuleHandle(res.smMissShadow_),
          },
  });

  res.smFullscreenVert_ = ctx_->createShaderModule({kCodeFullscreenVS, lvk::Stage_Vert, "Shader Module: fullscreen (vert)"});
  res.smFullscreenFrag_ = ctx_->createShaderModule({kCodeFullscreenFS, lvk::Stage_Frag, "Shader Module: fullscreen (frag)"});
  res.renderPipelineState_Fullscreen_ = ctx_->createRenderPipeline(lvk::RenderPipelineDesc{
      .smVert = res.smFullscreenVert_,
      .smFrag = res.smFullscreenFrag_,
      .color = {{.format = app.ctx_->getSwapchainFormat()}},
      .cullMode = lvk::CullMode_None,
      .debugName = "Pipeline: fullscreen",
  });

  if (!initModel(app.folderContentRoot_)) {
    VULKAN_APP_EXIT();
  }

  app.run([&](uint32_t width, uint32_t height, float aspectRatio, float deltaSeconds) {
    LVK_PROFILER_FUNCTION();

    lvk::ICommandBuffer& buffer = ctx_->acquireCommandBuffer();

    buffer.cmdUpdateBuffer(res.ubPerFrame_,
                           UniformsPerFrame{
                               .viewInverse = glm::inverse(app.camera_.getViewMatrix()),
                               .projInverse = glm::inverse(glm::perspective(float(45.0f * (M_PI / 180.0f)), aspectRatio, 0.5f, 500.0f)),
                           });

    // Pass 1: ray-trace the scene
    {
      struct {
        vec4 lightDir;
        uint64_t perFrame;
        uint64_t materials;
        uint64_t indices;
        uint64_t vertices;
        uint32_t outTexture;
        uint32_t tlas;
      } pc = {
          .lightDir = vec4(lightDir_, 0.0f),
          .perFrame = ctx_->gpuAddress(res.ubPerFrame_),
          .materials = ctx_->gpuAddress(res.sbMaterials_),
          .indices = ctx_->gpuAddress(res.ib0_),
          .vertices = ctx_->gpuAddress(res.vb0_),
          .outTexture = res.rayTracingOutputImage_.index(),
          .tlas = res.TLAS_.index(),
      };

      buffer.cmdBindRayTracingPipeline(res.rayTracingPipeline_);
      buffer.cmdPushConstants(pc);
      buffer.cmdTraceRays(width, height, 1, {.textures = {lvk::TextureHandle(res.rayTracingOutputImage_)}});
    }

    // Pass 2: render into the swapchain image
    {
      const lvk::RenderPass renderPassMain = {
          .color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}}},
      };
      const lvk::Framebuffer fbMain = {
          .color = {{.texture = ctx_->getCurrentSwapchainTexture()}},
      };
      buffer.cmdBeginRendering(renderPassMain, fbMain, {.textures = {lvk::TextureHandle(res.rayTracingOutputImage_)}});
      {
        buffer.cmdBindRenderPipeline(res.renderPipelineState_Fullscreen_);
        buffer.cmdPushDebugGroupLabel("Swapchain Output", 0xff0000ff);
        buffer.cmdBindDepthState({});
        struct {
          uint32_t texture;
        } bindings = {
            .texture = res.rayTracingOutputImage_.index(),
        };
        buffer.cmdPushConstants(bindings);
        buffer.cmdDraw(3);
        buffer.cmdPopDebugGroupLabel();

        app.imgui_->beginFrame(fbMain);
        app.drawFPS();
        app.imgui_->endFrame(buffer);
      }
      buffer.cmdEndRendering();
    }
    ctx_->submit(buffer, app.ctx_->getCurrentSwapchainTexture());
  });

  // destroy all the Vulkan stuff before closing the window
  res = {};

  VULKAN_APP_EXIT();
}
