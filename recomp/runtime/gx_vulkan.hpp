// GX on Vulkan 1.3 (gx_vulkan.cpp): the renderer behind GX's Sink.
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace qilin::rt::gx {

// Creates the renderer on the window (on the main thread) and makes it GX's
// sink. False, with the reason on stderr, if Vulkan 1.3 or a feature the
// renderer needs is missing: there is no fallback.
bool init_vulkan(SDL_Window* window);
// The window's drawable size changed: the swapchain is remade at the next frame.
void window_resized();
// The display lock is on at `speed` (host.cpp, whenever the display changes;
// 0: off): each frame's present is steered to land just before a vblank
// (Windows: the compositor's vblank times), by nudging the game's clock
// within 0.2% of that speed.
void lock_frames_to_vblank(double speed);
// The vblank lock's numbers for the run report (empty if it isn't on), and
// for the F3 readout: how long before its vblank a frame is presented (ms; 0
// without the lock).
std::string vblank_report();
double vblank_slack_ms();

// ---- the F3 readout: the time between frames shown ----
// A "low" is the frame rate of the average of the slowest 1% (or 0.1%) of
// frame times in the last 20 s: one bad frame in a thousand shows.
struct FrameTimeSummary {
    size_t frames = 0;  // in the window
    double average_ms = 0, fps = 0, low1_fps = 0, low01_fps = 0, worst_ms = 0;
};
FrameTimeSummary frame_time_summary();
std::vector<float> recent_frame_times(size_t n);  // the last n, oldest first (ms)
// The internal resolution the frame buffer has now (settings: 0 is from the
// screen's height, worked out at start).
uint32_t internal_scale();

// ---- for the settings menu (menu.cpp) ----
// What ImGui's Vulkan backend needs to draw into the window's images
// (Vulkan handles, as void*: this header doesn't include Vulkan's). False
// if the window's images can't be drawn into: no menu then.
struct OverlayTarget {
    void *instance, *physical_device, *device, *queue;
    uint32_t queue_family, images, format;
    void (*get_instance_proc_addr)();  // PFN_vkGetInstanceProcAddr, as the renderer loaded it
};
bool overlay_target(OverlayTarget& t);
// Something to draw over the picture in each frame (a VkCommandBuffer
// inside a rendering pass on the window's image); empty: nothing.
void set_overlay(std::function<void(void* command_buffer)> draw);
// While the game is parked (pause.hpp), on the main thread: a frame of the
// picture last shown with the overlay over it.
void present_paused();
// While the game is parked: the settings' internal resolution, anisotropic
// filtering and vsync take effect.
void apply_display_settings();
// The GPUs that can draw to the window, and the one in use.
const std::vector<std::string>& gpu_names();
std::string gpu_in_use();
// The GPU's anisotropic filtering limit (1: it has none).
float anisotropy_limit();

}  // namespace qilin::rt::gx
