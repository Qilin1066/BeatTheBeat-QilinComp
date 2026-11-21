// The recomp's settings menu, over the game, as the other Qilin recomps have
// it (Punch-Out's is the model): F1 or Escape (a controller: Back and Start
// together, which on their own are the Wii Remote's - and +) open and close
// it; Close and Quit sit at the top right of every page; it has the
// controls, the display and the game's pages; while it's open the game is
// paused, and closing it saves the settings. Alt+Enter or F11 switch full
// screen whether it's open or not. F3 shows the frame times in a corner. A
// hint shows how to open it for the first seconds of a run.
//
// Controls are set by pressing them: Add on a Wii Remote button's row, then
// the key, the controller's button or a click (an input taken from another
// button moves, and says so); clicking one removes it. "Look at the picture"
// hides the menu until any key or button.
//
// Dear ImGui (MIT) with its SDL3 backend for input and its Vulkan backend
// drawing into the window's images after the picture (gx_vulkan.hpp's
// overlay). Everything here runs on the main thread, except replaying the
// draw data, which the renderer's thread does. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;

namespace qilin::rt {

// The settings' display (SDL_DisplayID), or the main one if it isn't connected.
uint32_t chosen_display();

class Menu {
public:
    // After the renderer is up. Without a drawable window there is no menu
    // (said once); the game runs as before.
    explicit Menu(SDL_Window* window);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool ok() const { return ok_; }
    bool is_open() const { return open_; }
    // Every SDL event, before the runtime's own handling; true if the menu
    // took it (the game mustn't see it).
    bool handle(const SDL_Event& e);
    // Once per pass of the main loop: opens once the game has parked, draws
    // the open menu, the hint and the frame times.
    void tick();
    bool quit_requested() const { return quit_; }
    // Opening, open, or showing the hint or the frame times: the main loop
    // should tick often.
    bool busy() const;
    // Opens it (once the game parks) or closes it, as F1 does.
    void toggle();

private:
    void close();
    void draw_menu();
    void controls_page();
    void display_page();
    void game_page();
    void build_hint();
    void build_frame_times();
    void bind(int control, unsigned kind, uint32_t pad, const std::string& key);
    void save();

    SDL_Window* window_;
    bool ok_ = false, opening_ = false, open_ = false, quit_ = false, close_asked_ = false;
    bool sdl_backend_ = false, vulkan_backend_ = false;
    bool hint_ = false, peek_ = false;
    uint64_t hint_until_ = 0;  // SDL ticks (ms)
    // Setting a control by pressing it.
    int capture_ = -1;            // the control being set, or -1
    uint64_t capture_until_ = 0;  // SDL ticks
    uint32_t capture_held_ = 0;   // the controller's buttons held when it began (they don't count)
    bool wait_release_ = false;   // after one was taken: the mouse and the pad's navigation back once it's let go
    std::string note_;            // said under the table (what moved where)
    std::string report_note_;     // said under "Write a report"
};

}  // namespace qilin::rt
