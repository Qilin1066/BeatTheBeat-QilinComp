// The settings menu (menu.hpp). Runtime code: no std::format.
#include "menu.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "gx_vulkan.hpp"
#include "pause.hpp"
#include "settings.hpp"

namespace qilin::rt {

namespace {

constexpr uint64_t kHintMs = 8000;  // how long the hint shows at the start
constexpr const char* kHint = "F1 or Esc (controller: Back + Start): settings";

// ImGui's frame is built on the main thread and drawn by whichever thread
// owns the renderer at the time (the game's frames, or the main thread's
// while the game is parked): the lock keeps the two apart.
std::mutex g_draw_lock;
bool g_has_draw = false;  // under g_draw_lock: ImGui's draw data is a whole frame

// ImGui's Vulkan backend loads its functions through the renderer's loader.
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkVoidFunction load(const char* name, void*) { return g_gipa(g_instance, name); }

void check(VkResult r) {
    if (r < 0) std::fprintf(stderr, "qilin_run: settings menu: Vulkan error %d\n", int(r));
}

// The Wii Remote's buttons as the keyboard and a controller play them
// (host.cpp's tables), for the controls page.
struct Binding {
    const char *button, *keys, *pad;
};
constexpr Binding kBindings[] = {
    {"A", "A, Right Shift, Right arrow, Space", "South (A / Cross), right shoulder"},
    {"B", "B, Left Shift, Left arrow", "East (B / Circle), left shoulder, right trigger"},
    {"1", "1", "West (X / Square)"},
    {"2", "2", "North (Y / Triangle)"},
    {"+", "Enter", "Start"},
    {"-", "Backspace, Tab", "Back"},
    {"Home", "H", "Guide"},
    {"D-pad", "I, J, K, L; Up and Down arrows", "D-pad, left stick"},
};

constexpr const char* kLanguages[] = {"Japanese", "English", "German", "French", "Spanish", "Italian"};

std::string display_name(SDL_DisplayID id) {
    const char* n = SDL_GetDisplayName(id);
    return n ? n : "";
}

// Centres the window on a display; full screen moves with it.
void move_to_display(SDL_Window* w, SDL_DisplayID id) {
    const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    if (full) SDL_SetWindowFullscreen(w, false);
    const int at = int(SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetWindowPosition(w, at, at);
    if (full) SDL_SetWindowFullscreen(w, true);
}

}  // namespace

uint32_t chosen_display() {
    const std::string& want = settings().monitor;
    if (!want.empty()) {
        int n = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
            SDL_DisplayID found = 0;
            for (int i = 0; i < n && !found; ++i)
                if (display_name(ids[i]) == want) found = ids[i];
            SDL_free(ids);
            if (found) return found;
        }
        std::fprintf(stderr, "qilin_run: the settings' display \"%s\" isn't connected; using the main one\n", want.c_str());
    }
    return SDL_GetPrimaryDisplay();
}

Menu::Menu(SDL_Window* window) : window_(window) {
    gx::OverlayTarget t{};
    if (!gx::overlay_target(t)) {
        std::fprintf(stderr, "qilin_run: the window's images can't be drawn into: no settings menu\n");
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the settings file is the only file
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // 1.25x, as the other recomps have it, times the display's own scale.
    float dpi = SDL_GetWindowDisplayScale(window);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ImGui::GetStyle().ScaleAllSizes(1.25f * dpi);
    ImGui::GetStyle().FontScaleMain = 1.25f;
    ImGui::GetStyle().FontScaleDpi = dpi;

    sdl_backend_ = ImGui_ImplSDL3_InitForVulkan(window);
    g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(t.get_instance_proc_addr);
    g_instance = static_cast<VkInstance>(t.instance);
    if (!sdl_backend_ || !ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load)) {
        std::fprintf(stderr, "qilin_run: the settings menu can't start (its SDL or Vulkan side)\n");
        return;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_instance;
    ii.PhysicalDevice = static_cast<VkPhysicalDevice>(t.physical_device);
    ii.Device = static_cast<VkDevice>(t.device);
    ii.QueueFamily = t.queue_family;
    ii.Queue = static_cast<VkQueue>(t.queue);
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    ii.MinImageCount = std::max(2u, t.images);
    ii.ImageCount = ii.MinImageCount;
    ii.UseDynamicRendering = true;
    static VkFormat format;  // the backend keeps a pointer to it
    format = static_cast<VkFormat>(t.format);
    VkPipelineRenderingCreateInfoKHR& pr = ii.PipelineInfoMain.PipelineRenderingCreateInfo;
    pr = {};
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pr.colorAttachmentCount = 1, pr.pColorAttachmentFormats = &format;
    ii.CheckVkResultFn = check;
    vulkan_backend_ = ImGui_ImplVulkan_Init(&ii);
    if (!vulkan_backend_) {
        std::fprintf(stderr, "qilin_run: the settings menu can't start (its Vulkan side)\n");
        return;
    }
    gx::set_overlay([](void* cb) {
        std::lock_guard l(g_draw_lock);
        if (g_has_draw) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cb));
    });
    ok_ = true;
    hint_ = true;
    hint_until_ = SDL_GetTicks() + kHintMs;
    std::fprintf(stderr, "qilin_run: %s\n", kHint);
}

Menu::~Menu() {
    gx::set_overlay(nullptr);
    if (vulkan_backend_) ImGui_ImplVulkan_Shutdown();
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

bool Menu::handle(const SDL_Event& e) {
    if (!ok_) return false;
    const bool shown = open_ || opening_;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) return false;  // full screen: the host's
        if (!e.key.repeat && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) {
            toggle();
            return true;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        // Back and Start together (alone they're the remote's - and +);
        // with the menu open, Start alone closes it too.
        SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
        const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
            toggle();
            return true;
        }
        break;
    }
    default: break;
    }
    if (open_) ImGui_ImplSDL3_ProcessEvent(&e);
    if (!shown) return false;
    switch (e.type) {  // the game doesn't see input while the menu is up
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: return true;
    default: return false;
    }
}

void Menu::toggle() {
    if (open_ || opening_) {
        close();
        return;
    }
    opening_ = true;
    request_pause(true);  // the game parks at the end of its frame; tick() opens then
}

void Menu::close() {
    if (open_) {
        if (!save_settings()) std::fprintf(stderr, "qilin_run: the settings can't be saved to %s\n", settings_path().string().c_str());
        gx::apply_display_settings();  // the game is still parked: the renderer is ours
    }
    open_ = opening_ = false;
    {
        std::lock_guard l(g_draw_lock);
        g_has_draw = false;
    }
    request_pause(false);
}

void Menu::tick() {
    if (!ok_) return;
    if (opening_ && game_parked()) {
        opening_ = false, open_ = true, hint_ = false;
        ImGui::GetIO().ClearInputKeys();
    }
    if (open_) {
        {
            std::lock_guard l(g_draw_lock);
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            draw_menu();
            ImGui::Render();
            g_has_draw = true;
        }
        if (close_asked_ || quit_) {
            close_asked_ = false;
            if (!quit_) close();
            return;
        }
        gx::present_paused();
        return;
    }
    if (hint_) {
        std::lock_guard l(g_draw_lock);
        if (SDL_GetTicks() >= hint_until_) {
            hint_ = false;
            g_has_draw = false;
            return;
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        build_hint();
        ImGui::Render();
        g_has_draw = true;
    }
}

bool Menu::busy() const { return open_ || opening_ || hint_; }

void Menu::build_hint() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + vp->WorkSize.y - pad), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(kHint);
    ImGui::End();
}

void Menu::draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Beat the Beat: Rhythm Paradise - settings (the game is paused)");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Close (Esc)").x);
    if (ImGui::Button("Close (Esc)")) close_asked_ = true;  // tick() closes it, outside the draw lock
    ImGui::Separator();
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Controls")) {
            controls_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            game_page();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Menu::controls_page() {
    Settings& s = settings();
    ImGui::SeparatorText("Controllers");
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (n == 0) ImGui::TextDisabled("None connected: the keyboard plays the Wii Remote.");
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        ImGui::BulletText("%s", name ? name : "A controller");  // each one plays the Wii Remote
    }
    SDL_free(ids);
    ImGui::Checkbox("Rumble", &s.rumble);
    ImGui::SeparatorText("The Wii Remote's buttons");
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (const Binding& b : kBindings) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(), ImGui::TextUnformatted(b.button);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(b.keys);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(b.pad);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F11 or Alt+Enter: full screen.");
}

void Menu::display_page() {
    Settings& s = settings();
    ImGui::SeparatorText("Window");
    bool full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &full)) {
        SDL_SetWindowFullscreen(window_, full);
        s.fullscreen = full;
    }
    {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        const std::string current = s.monitor.empty() ? "The main display" : s.monitor;
        if (ImGui::BeginCombo("Display", current.c_str())) {
            if (ImGui::Selectable("The main display", s.monitor.empty())) {
                s.monitor.clear();
                move_to_display(window_, SDL_GetPrimaryDisplay());
            }
            for (int i = 0; i < n; ++i) {
                const std::string name = display_name(ids[i]);
                ImGui::PushID(i);
                if (ImGui::Selectable(name.c_str(), name == s.monitor)) {
                    s.monitor = name;
                    move_to_display(window_, ids[i]);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        SDL_free(ids);
    }

    ImGui::SeparatorText("Picture");
    int mode = int(s.scaling);
    ImGui::RadioButton("Whole multiples (sharpest)", &mode, int(Scaling::integer));
    ImGui::SameLine();
    ImGui::RadioButton("Fit", &mode, int(Scaling::fit));
    ImGui::SameLine();
    ImGui::RadioButton("Fill the window", &mode, int(Scaling::stretch));
    s.scaling = Scaling(mode);
    ImGui::Checkbox("Smooth", &s.smooth);
    ImGui::SameLine();
    ImGui::Checkbox("Vsync", &s.vsync);

    ImGui::SeparatorText("Timing");
    ImGui::BeginDisabled(!s.vsync);
    ImGui::Checkbox("Lock the game to the display's refresh (from the next start)", &s.lock_to_display);
    ImGui::Checkbox("Fullscreen at 60/120 Hz when the desktop's refresh doesn't suit (from the next start)", &s.match_refresh);
    ImGui::EndDisabled();
    int latency = int(s.audio_latency_ms);
    if (ImGui::SliderInt("Sound latency", &latency, 5, 200, "%d ms", ImGuiSliderFlags_AlwaysClamp))
        s.audio_latency_ms = uint32_t(latency);
    ImGui::TextDisabled("Lower: less lag between a press and its sound (held no lower than the device can keep up). Raise it if the sound crackles.");
#if defined(_WIN32)
    ImGui::Checkbox("Exclusive sound: the device for the game alone, less lag (from the next start)", &s.exclusive_audio);
#endif

    ImGui::SeparatorText("Enhancements");
    static constexpr const char* kPresets[] = {"original", "low-end", "desktop"};
    if (ImGui::BeginCombo("Preset", s.preset.c_str())) {
        for (const char* p : kPresets)
            if (ImGui::Selectable(p, s.preset == p)) apply_preset(s, p);
        ImGui::EndCombo();
    }
    int scale = int(s.internal_scale);
    char label[64];
    std::snprintf(label, sizeof label, "%dx (%dx%d)", scale, 640 * scale, 528 * scale);
    if (ImGui::SliderInt("Internal resolution", &scale, 1, 8, label, ImGuiSliderFlags_AlwaysClamp)) s.internal_scale = uint32_t(scale);
    const float limit = gx::anisotropy_limit();
    ImGui::BeginDisabled(limit <= 1.0f);
    static constexpr uint32_t kAniso[] = {1, 2, 4, 8, 16};
    std::snprintf(label, sizeof label, "%ux", s.anisotropy);
    if (ImGui::BeginCombo("Anisotropic filtering", label)) {
        for (uint32_t a : kAniso) {
            if (float(a) > limit && a > 1) continue;
            std::snprintf(label, sizeof label, a == 1 ? "%ux (the console's)" : "%ux", a);
            if (ImGui::Selectable(label, s.anisotropy == a)) s.anisotropy = a;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (limit <= 1.0f) ImGui::TextDisabled("This GPU has no anisotropic filtering.");

    ImGui::SeparatorText("Graphics device");
    const std::string in_use = gx::gpu_in_use();
    const std::string chosen = s.gpu.empty() ? "The best one" : s.gpu;
    if (ImGui::BeginCombo("Device", chosen.c_str())) {
        if (ImGui::Selectable("The best one", s.gpu.empty())) s.gpu.clear();
        for (const std::string& name : gx::gpu_names())
            if (ImGui::Selectable(name.c_str(), name == s.gpu)) s.gpu = name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("In use: %s. A change takes effect the next time the game starts.", in_use.c_str());
    ImGui::TextDisabled("The rest takes effect when this menu closes.");
}

void Menu::game_page() {
    Settings& s = settings();
    ImGui::SeparatorText("The Wii's own settings (read the next time the game starts)");
    int lang = std::clamp(int(s.language), 1, 5);  // this disc has English, German, French, Spanish and Italian
    if (ImGui::BeginCombo("Language", kLanguages[lang])) {
        for (int i = 1; i <= 5; ++i)
            if (ImGui::Selectable(kLanguages[i], i == lang)) s.language = uint8_t(i);
        ImGui::EndCombo();
    }
    ImGui::Checkbox("60 Hz (EuRGB60; off: PAL 50 Hz)", &s.sixty_hz);
    ImGui::Checkbox("Widescreen (16:9)", &s.widescreen);
    ImGui::Checkbox("Progressive scan", &s.progressive);
    // The game plays surround as Dolby Pro Logic II (dpl2.hpp), for a
    // receiver or TV that decodes it.
    static constexpr const char* kSound[] = {"Mono", "Stereo", "Surround (Dolby Pro Logic II)"};
    int sound = std::clamp(int(s.sound_mode), 0, 2);
    if (ImGui::Combo("Sound", &sound, kSound, 3)) s.sound_mode = uint8_t(sound);
    ImGui::Checkbox("Sensor bar above the TV", &s.sensor_bar_above);
    int dpd = std::clamp(int(s.dpd_sensitivity), 1, 5);
    if (ImGui::SliderInt("Pointer sensitivity", &dpd, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp)) s.dpd_sensitivity = uint8_t(dpd);

    ImGui::SeparatorText("About");
    ImGui::TextWrapped("Settings file: %s", settings_path().string().c_str());
    ImGui::TextWrapped("A static recompilation: the game's own code, compiled for this computer. "
                       "Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT).");
    ImGui::Spacing();
    if (ImGui::Button("Quit the game")) {
        if (!save_settings()) std::fprintf(stderr, "qilin_run: the settings can't be saved to %s\n", settings_path().string().c_str());
        quit_ = true;
    }
}

}  // namespace qilin::rt
