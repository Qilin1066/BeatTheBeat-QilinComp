// GX on Vulkan 1.3 (gx_vulkan.cpp): the renderer behind GX's Sink.
// Runtime code: no std::format.
#pragma once

struct SDL_Window;

namespace qilin::rt::gx {

// Creates the renderer on the window (on the main thread) and makes it GX's
// sink. False, with the reason on stderr, if Vulkan 1.3 or a feature the
// renderer needs is missing: there is no fallback.
bool init_vulkan(SDL_Window* window);
// The window's drawable size changed: the swapchain is remade at the next frame.
void window_resized();

}  // namespace qilin::rt::gx
