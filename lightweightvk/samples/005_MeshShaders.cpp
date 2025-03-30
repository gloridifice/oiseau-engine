/*
 * LightweightVK
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VulkanApp.h"

// we are going to use raw Vulkan here to initialize VK_EXT_mesh_shader
#include <lvk/vulkan/VulkanUtils.h>

const char* codeTask = R"(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

void main() {
  EmitMeshTasksEXT(1, 1, 1);
}
)";

const char* codeMesh = R"(
layout(triangles, max_vertices = 3, max_primitives = 1) out;

layout (location=0) out vec3 color[3];

const vec2 pos[3] = vec2[3](
  vec2(-0.6, -0.4),
  vec2( 0.6, -0.4),
  vec2( 0.0,  0.6)
);
const vec3 col[3] = vec3[3](
  vec3(1.0, 0.0, 0.0),
  vec3(0.0, 1.0, 0.0),
  vec3(0.0, 0.0, 1.0)
);
void main() {
  SetMeshOutputsEXT(3, 1);

  for (uint i = 0; i != 3; i++) {
    gl_MeshVerticesEXT[i].gl_Position = vec4(pos[i], 0.0, 1.0);
    color[i] = col[i];
  }

  gl_MeshPrimitivesEXT[0].gl_CullPrimitiveEXT = false;
  gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
)";

const char* codeFrag = R"(
#version 460
layout (location=0) in vec3 color;
layout (location=0) out vec4 out_FragColor;

void main() {
  out_FragColor = vec4(color, 1.0);
};
)";

struct {
  lvk::Holder<lvk::RenderPipelineHandle> renderPipelineState_Triangle_;
  lvk::Holder<lvk::ShaderModuleHandle> task_;
  lvk::Holder<lvk::ShaderModuleHandle> mesh_;
  lvk::Holder<lvk::ShaderModuleHandle> frag_;
} res;

VULKAN_APP_MAIN {
  VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
      .taskShader = VK_TRUE,
      .meshShader = VK_TRUE,
  };
  const VulkanAppConfig cfg{
      .width = 800,
      .height = 600,
      .resizable = true,
      .contextConfig =
          {
              .extensionsDevice = {"VK_EXT_mesh_shader"},
              .extensionsDeviceFeatures = &meshShaderFeatures,
          },
  };
  VULKAN_APP_DECLARE(app, cfg);

  lvk::IContext* ctx = app.ctx_.get();

  res.task_ = ctx->createShaderModule({codeTask, lvk::Stage_Task, "Shader Module: main (task)"});
  res.mesh_ = ctx->createShaderModule({codeMesh, lvk::Stage_Mesh, "Shader Module: main (mesh)"});
  res.frag_ = ctx->createShaderModule({codeFrag, lvk::Stage_Frag, "Shader Module: main (frag)"});

  res.renderPipelineState_Triangle_ = ctx->createRenderPipeline(
      {
          .smTask = res.task_,
          .smMesh = res.mesh_,
          .smFrag = res.frag_,
          .color = {{.format = ctx->getSwapchainFormat()}},
      },
      nullptr);

  LVK_ASSERT(res.renderPipelineState_Triangle_.valid());

  app.run([&](uint32_t width, uint32_t height, float aspectRatio, float deltaSeconds) {
    lvk::ICommandBuffer& buffer = ctx->acquireCommandBuffer();

    // This will clear the framebuffer
    buffer.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .clearColor = {1.0f, 1.0f, 1.0f, 1.0f}}}},
                             {.color = {{.texture = ctx->getCurrentSwapchainTexture()}}});
    buffer.cmdBindRenderPipeline(res.renderPipelineState_Triangle_);
    buffer.cmdPushDebugGroupLabel("Render Triangle", 0xff0000ff);
    buffer.cmdDrawMeshTasks({1, 1, 1});

    buffer.cmdPopDebugGroupLabel();
    buffer.cmdEndRendering();
    ctx->submit(buffer, ctx->getCurrentSwapchainTexture());
  });

  // destroy all the Vulkan stuff before closing the window
  res = {};

  VULKAN_APP_EXIT();
}
