#pragma once

#include <memory>

namespace ui::hud
{
// The overlay's HUD panels (watermark, spectators, event log, notices, binds, bomb). Each can be dragged while the
// menu is open (`interactive`); their positions are saved in the configuration. Render thread only.
class panels
{
public:
    panels();
    ~panels();
    panels(const panels&) = delete;
    panels& operator=(const panels&) = delete;

    void draw_watermark(bool interactive);
    void draw_spectator_list(bool interactive);
    void draw_event_log(bool interactive);
    void draw_system_notices(bool interactive);
    void draw_keybind_list(bool interactive);
    void draw_bomb_info(bool interactive);
    void draw_movement_info(bool interactive);

private:
    // Samplers, drag state and animation kept between frames.
    struct state;
    std::unique_ptr<state> m_state;
};
} // namespace ui::hud
