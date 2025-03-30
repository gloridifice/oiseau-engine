//
// Created by Koiro on 30/03/2025.
//
#include "glfw/glfw3.h"
#include "lvk/LVK.h"

int main(int argc, char* argv[]) {
  minilog::initialize(nullptr, {.threadNames = false});
  int width = 960;
  int height = 540;
  GLFWwindow* window = lvk::initWindow("Oiseau Engine", width, height);
  auto ctx = lvk::createVulkanContextWithSwapchain(window, width, height, {});

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    glfwGetFramebufferSize(window, &width, &height);
    if (width == 0 || height == 0)
      continue;
    lvk::ICommandBuffer& buf = ctx->acquireCommandBuffer();
    ctx->submit(buf, ctx->getCurrentSwapchainTexture());
  }

  ctx.reset();
  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
