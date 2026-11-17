#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <platform/windows/input.hpp>
#include <diagnostics/health.hpp>
#include <render/presentation.hpp>
#include <game/memory/bound_services.hpp>
#include <game/state/services.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <ui/presentation.hpp>
#include <features/misc/auto_stop.hpp>
#include <features/visuals/event_log.hpp>
#include <features/visuals/visuals.hpp>
#include <render/text/localization.hpp>
#include <ui/overlay/hud.hpp>
#include <ui/overlay/overlay.hpp>
#include <ui/overlay/system_notices.hpp>
#include <resources/watermark_loss_icon.hpp>
#include <imgui.h>
#include <ui/menu/menu.hpp>

namespace ui::hud
{
namespace
{
constexpr ImU32 k_wm_bg = IM_COL32(13, 13, 18, 140);
constexpr ImU32 k_wm_border = IM_COL32(255, 255, 255, 18);
// Notices must stay readable over any game scene, so their body is more opaque than the watermark's.
constexpr ImU32 k_wm_bg_solid = IM_COL32(13, 13, 18, 226);
constexpr ImU32 k_wm_text = IM_COL32(248, 248, 242, 255);
constexpr ImU32 k_wm_muted = IM_COL32(150, 150, 160, 255);

// Uppercase, matching the sidebar wordmark.
constexpr const char* k_wm_brand = "VESTA";

enum class stat_icon
{
    ping,
    loss,
    cpu
};

void draw_stat_icon(ImDrawList* dl, stat_icon icon, float x, float y, float size, ImU32 color)
{
    if (!dl || size <= 0.0f)
        return;

    if (icon == stat_icon::cpu)
    {
        // Original CPU chip: rounded body plus three pill-shaped pins per side.
        constexpr float body_inset = 0.1822f;
        constexpr float pin_w = 0.1018f;
        constexpr float pin_len = 0.1332f;
        constexpr float pin_centers[3]{0.5f - 0.1657f, 0.5f, 0.5f + 0.1657f};
        const ImVec2 body_min{x + size * body_inset, y + size * body_inset};
        const ImVec2 body_max{x + size * (1.0f - body_inset), y + size * (1.0f - body_inset)};
        const auto pin_width = size * pin_w;
        const auto pin_length = size * pin_len;
        const auto pin_rounding = pin_width * 0.5f;

        for (const auto center : pin_centers)
        {
            const auto pin_x = x + size * center - pin_width * 0.5f;
            const auto pin_y = y + size * center - pin_width * 0.5f;
            dl->AddRectFilled({pin_x, y}, {pin_x + pin_width, y + pin_length}, color, pin_rounding);
            dl->AddRectFilled({pin_x, y + size - pin_length}, {pin_x + pin_width, y + size}, color, pin_rounding);
            dl->AddRectFilled({x, pin_y}, {x + pin_length, pin_y + pin_width}, color, pin_rounding);
            dl->AddRectFilled({x + size - pin_length, pin_y}, {x + size, pin_y + pin_width}, color, pin_rounding);
        }
        dl->AddRectFilled(body_min, body_max, color, size * 0.053f);
        return;
    }

    if (icon == stat_icon::ping)
    {
        // Segoe MobSignal5 reference: four equally spaced, ascending bars.
        constexpr std::array<float, 4> heights{0.24f, 0.42f, 0.60f, 0.78f};
        constexpr auto left = 0.10f;
        constexpr auto bottom = 0.88f;
        constexpr auto bar_width = 0.135f;
        constexpr auto gap = 0.095f;
        for (std::size_t index{}; index < heights.size(); ++index)
        {
            const auto bar_x = x + size * (left + static_cast<float>(index) * (bar_width + gap));
            const auto bar_bottom = y + size * bottom;
            const auto bar_top = bar_bottom - size * heights[index];
            const auto width = size * bar_width;
            dl->AddRectFilled({bar_x, bar_top}, {bar_x + width, bar_bottom}, color, width * 0.46f);
        }
        return;
    }

    if (icon == stat_icon::loss)
    {
        const auto to_screen = [=](const float px, const float py) { return ImVec2{x + px * size, y + py * size}; };
        for (const auto& contour : resources::icons::watermark_loss_contours)
        {
            dl->PathClear();
            for (auto offset = std::uint16_t{}; offset < contour.count; ++offset)
            {
                const auto& command = resources::icons::watermark_loss_commands[contour.first + offset];
                switch (command.op)
                {
                    case resources::icons::glyph_path_op::move:
                    case resources::icons::glyph_path_op::line:
                        dl->PathLineTo(to_screen(command.x1, command.y1));
                        break;
                    case resources::icons::glyph_path_op::quadratic:
                        dl->PathBezierQuadraticCurveTo(to_screen(command.x1, command.y1),
                                                       to_screen(command.x2, command.y2), 4);
                        break;
                    case resources::icons::glyph_path_op::cubic:
                        dl->PathBezierCubicCurveTo(to_screen(command.x1, command.y1), to_screen(command.x2, command.y2),
                                                   to_screen(command.x3, command.y3), 5);
                        break;
                }
            }
            dl->PathFillConcave(color);
        }
    }
}

// Share of all logical CPUs used by this process, refreshed twice a second.
class cpu_sampler
{
public:
    float sample()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_next)
            return m_value;
        m_next = now + std::chrono::milliseconds(500);

        FILETIME create_ft{}, exit_ft{}, kernel_ft{}, user_ft{};
        if (!::GetProcessTimes(::GetCurrentProcess(), &create_ft, &exit_ft, &kernel_ft, &user_ft))
            return m_value;
        FILETIME wall_ft{};
        ::GetSystemTimeAsFileTime(&wall_ft);

        const auto cpu = to_ticks(kernel_ft) + to_ticks(user_ft);
        const auto wall = to_ticks(wall_ft);
        if (m_primed && wall > m_previous_wall)
        {
            const auto cpu_delta = static_cast<double>(cpu - m_previous_cpu);
            const auto wall_delta = static_cast<double>(wall - m_previous_wall);
            m_value = static_cast<float>(std::clamp(cpu_delta / wall_delta / m_cpu_count * 100.0, 0.0, 100.0));
        }
        m_previous_cpu = cpu;
        m_previous_wall = wall;
        m_primed = true;
        return m_value;
    }

private:
    static std::uint64_t to_ticks(const FILETIME& time) noexcept
    {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    static int logical_cpus() noexcept
    {
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        return std::max(1, static_cast<int>(info.dwNumberOfProcessors));
    }

    int m_cpu_count{logical_cpus()};
    std::uint64_t m_previous_cpu{};
    std::uint64_t m_previous_wall{};
    bool m_primed{};
    float m_value{};
    std::chrono::steady_clock::time_point m_next{};
};

// Scoreboard ping of the local controller, refreshed four times a second.
class ping_sampler
{
public:
    int sample()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_next)
            return m_value;
        m_next = now + std::chrono::milliseconds(250);

        const auto controller = game::local_player().controller();
        m_value = controller ? std::clamp(game::bound_memory().process.load<std::int32_t>(
                                              controller + SCHEMA("CCSPlayerController", "m_iPing"_id)),
                                          0, 999)
                             : 0;
        return m_value;
    }

private:
    int m_value{};
    std::chrono::steady_clock::time_point m_next{};
};

// Connection instability: the spread of recent ping samples relative to their mean, as 0..100.
class instability_sampler
{
public:
    float sample(int ping)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_next)
            return m_value;
        m_next = now + std::chrono::milliseconds(250);

        if (!m_primed)
        {
            m_samples.fill(ping);
            m_primed = true;
        }
        m_samples[m_head] = ping;
        m_head = (m_head + 1) % m_samples.size();

        auto mean = 0.0f;
        for (const auto sample : m_samples)
            mean += static_cast<float>(sample);
        mean /= static_cast<float>(m_samples.size());
        auto variance = 0.0f;
        for (const auto sample : m_samples)
        {
            const auto deviation = static_cast<float>(sample) - mean;
            variance += deviation * deviation;
        }
        variance /= static_cast<float>(m_samples.size());

        m_value = std::clamp(std::sqrt(variance) / mean * 140.0f, 0.0f, 100.0f);
        if (!std::isfinite(m_value))
            m_value = 0.0f;
        return m_value;
    }

private:
    std::array<int, 16> m_samples{};
    std::size_t m_head{};
    bool m_primed{};
    float m_value{};
    std::chrono::steady_clock::time_point m_next{};
};

// Frames drawn by the overlay per second, counted over half-second windows.
class frame_rate_counter
{
public:
    int tick()
    {
        ++m_frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - m_window_start >= std::chrono::milliseconds(500))
        {
            m_value = m_frames * 2;
            m_frames = 0;
            m_window_start = now;
        }
        return m_value;
    }

private:
    int m_frames{};
    int m_value{};
    std::chrono::steady_clock::time_point m_window_start{std::chrono::steady_clock::now()};
};

// Green below the low threshold, amber up to the high one, red beyond.
ImU32 threshold_color(float value, float low, float high)
{
    if (value <= low)
        return k_wm_text;
    if (value >= high)
        return IM_COL32(235, 70, 70, 255);
    const auto t = (value - low) / (high - low);
    if (t < 0.5f)
        return IM_COL32(235, 200, 90, 255);
    return IM_COL32(235, 130, 70, 255);
}

ImU32 loss_icon_color(float loss, float low, float high)
{
    if (loss <= low)
        return k_wm_muted;
    if (loss >= high)
        return IM_COL32(235, 70, 70, 255);
    const auto t = (loss - low) / (high - low);
    const auto lerp = [](int a, int b, float u) { return static_cast<int>(a + static_cast<float>(b - a) * u); };
    return IM_COL32(lerp(150, 235, t), lerp(150, 70, t), lerp(160, 70, t), 255);
}

[[nodiscard]] float overlay_dpi_scale()
{
    return ui::bound_overlay().ui_dpi_scale();
}

[[nodiscard]] ImVec2 physical_mouse_position()
{
    POINT cursor{};
    const auto window = ui::bound_overlay().hwnd();
    if (window && ::GetCursorPos(&cursor) && ::ScreenToClient(window, &cursor))
    {
        return {static_cast<float>(cursor.x), static_cast<float>(cursor.y)};
    }

    auto mouse = ImGui::GetIO().MousePos;
    const auto display = ImGui::GetIO().DisplaySize;
    ui::bound_menu().map_pointer_to_screen(mouse.x, mouse.y, display.x, display.y);
    return mouse;
}

class screen_ui_space final
{
public:
    screen_ui_space()
    {
        m_display = ImGui::GetIO().DisplaySize;
        const auto& overlay = ui::bound_overlay();
        if (!overlay.ui_uses_fullscreen_canvas() || overlay.ui_reference_width() == 0 ||
            overlay.ui_reference_height() == 0)
        {
            return;
        }
        m_position_scale = {m_display.x / std::max(1.0f, static_cast<float>(overlay.ui_reference_width())),
                            m_display.y / std::max(1.0f, static_cast<float>(overlay.ui_reference_height()))};
    }

    [[nodiscard]] ImVec2 display() const noexcept { return m_display; }
    [[nodiscard]] ImVec2 position_scale() const noexcept { return m_position_scale; }

private:
    ImVec2 m_display{};
    ImVec2 m_position_scale{1.0f, 1.0f};
};

[[nodiscard]] ImVec2 clamp_to_display(const ImVec2 position, const ImVec2 size, const ImVec2 display)
{
    return {std::clamp(position.x, 0.0f, std::max(0.0f, display.x - size.x)),
            std::clamp(position.y, 0.0f, std::max(0.0f, display.y - size.y))};
}

// Moves a HUD panel with the left button and, when given its scale, resizes it with the wheel. Only active while the
// menu is open and owns the mouse.
class panel_drag
{
public:
    // `origin` and `size` describe the panel as last laid out; returns the dragged origin, or `origin` when idle.
    ImVec2 update(const bool interactive, const ImVec2 origin, const ImVec2 size, float* scale = nullptr)
    {
        if (!interactive)
            return origin;
        const auto& io = ImGui::GetIO();
        const auto mouse = physical_mouse_position();
        const bool inside =
            mouse.x >= origin.x && mouse.x <= origin.x + size.x && mouse.y >= origin.y && mouse.y <= origin.y + size.y;
        if (scale && inside && !m_active && std::abs(io.MouseWheel) > 0.001f)
            *scale = std::clamp(*scale + io.MouseWheel * 0.08f, 0.55f, 2.0f);
        if (io.MouseDown[0] && !m_active && inside)
        {
            m_active = true;
            m_grab = {mouse.x - origin.x, mouse.y - origin.y};
        }
        if (!io.MouseDown[0])
            m_active = false;
        return m_active ? ImVec2{mouse.x - m_grab.x, mouse.y - m_grab.y} : origin;
    }

    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    bool m_active{};
    ImVec2 m_grab{};
};

// A font of the overlay atlas with its size, falling back to ImGui's default font.
struct hud_font
{
    ImFont* font{};
    float size{};
};

[[nodiscard]] hud_font resolve_font(const render::draw::font* wrap, const float scale)
{
    if (wrap && wrap->im_font)
        return {wrap->im_font, wrap->font_size * scale};
    return {ImGui::GetFont(), ImGui::GetFontSize() * scale};
}

[[nodiscard]] ImU32 with_alpha(const ImU32 color, const float factor)
{
    const auto alpha =
        static_cast<ImU32>(std::lround(static_cast<float>(color >> 24) * std::clamp(factor, 0.0f, 1.0f)));
    return (color & 0x00ffffffu) | (alpha << 24);
}

// Soft drop shadow cast to the bottom right of a panel.
void draw_panel_shadow(ImDrawList* draw, const ImVec2 min, const ImVec2 max, const float rounding, const float step,
                       const float strength)
{
    for (int layer = 5; layer >= 1; --layer)
    {
        const auto offset = static_cast<float>(layer) * step;
        const auto alpha = static_cast<int>(strength * (1.0f - static_cast<float>(layer) / 6.0f));
        draw->AddRectFilled({min.x + offset, min.y + offset}, {max.x + offset, max.y + offset},
                            IM_COL32(0, 0, 0, alpha), rounding);
    }
}

// Everything the HUD keeps between frames. All of it is touched on the render thread only.
struct hud_state
{
    frame_rate_counter frame_rate;
    cpu_sampler cpu;
    ping_sampler ping;
    instability_sampler instability;
    panel_drag watermark;
    ImVec2 watermark_size{};
    panel_drag spectators;
    panel_drag events;
    panel_drag keybinds;
    panel_drag bomb;
    panel_drag movement;
    ui::notices::dismissals closed_notices;
    std::uint64_t loading_serial{};
    std::chrono::steady_clock::time_point loading_since{};
};

struct compact_panel_row
{
    std::string text{};
    ImU32 color{k_wm_text};
};

void draw_compact_panel(const char* title, const std::vector<compact_panel_row>& rows,
                        config::general_profile::screen_layout& layout, panel_drag& drag, bool interactive);

[[nodiscard]] ImVec2 resolve_screen_layout(const config::general_profile::screen_layout& layout,
                                           const ImVec2 panel_size, const ImVec2 display, const float dpi_scale,
                                           const ImVec2 position_scale)
{
    const auto x_offset = std::max(0.0f, layout.offset_x) * dpi_scale * position_scale.x;
    const auto y_offset = std::max(0.0f, layout.offset_y) * dpi_scale * position_scale.y;
    float x = x_offset;
    float y = y_offset;
    using anchor = config::general_profile::screen_anchor;
    if (layout.anchor == anchor::top_right || layout.anchor == anchor::bottom_right)
    {
        x = display.x - panel_size.x - x_offset;
    }
    if (layout.anchor == anchor::bottom_left || layout.anchor == anchor::bottom_right)
    {
        y = display.y - panel_size.y - y_offset;
    }
    return {std::clamp(x, 0.0f, std::max(0.0f, display.x - panel_size.x)),
            std::clamp(y, 0.0f, std::max(0.0f, display.y - panel_size.y))};
}

void capture_screen_layout(config::general_profile::screen_layout& layout, const ImVec2 position,
                           const ImVec2 panel_size, const ImVec2 display, const float dpi_scale,
                           const ImVec2 position_scale)
{
    using anchor = config::general_profile::screen_anchor;
    const bool right = position.x + panel_size.x * 0.5f > display.x * 0.5f;
    const bool bottom = position.y + panel_size.y * 0.5f > display.y * 0.5f;
    layout.anchor =
        bottom ? (right ? anchor::bottom_right : anchor::bottom_left) : (right ? anchor::top_right : anchor::top_left);
    const auto safe_x = std::max(0.01f, dpi_scale * position_scale.x);
    const auto safe_y = std::max(0.01f, dpi_scale * position_scale.y);
    layout.offset_x = std::max(0.0f, right ? display.x - position.x - panel_size.x : position.x) / safe_x;
    layout.offset_y = std::max(0.0f, bottom ? display.y - position.y - panel_size.y : position.y) / safe_y;
    layout.version = 1;
}

void migrate_screen_layout(config::general_profile::screen_layout& layout, const ImVec2 panel_size,
                           const ImVec2 display, const float dpi_scale, const ImVec2 position_scale)
{
    if (layout.version >= 1)
        return;
    capture_screen_layout(layout, {layout.legacy_position_x, layout.legacy_position_y}, panel_size, display, dpi_scale,
                          position_scale);
}

[[nodiscard]] float text_width(ImFont* font, const float size, const char* text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

// Baseline that centers the visible ink of `parts` (not the font's line box) vertically in a box.
[[nodiscard]] float ink_centered_y(ImFont* font, const float size, const std::initializer_list<const char*> parts,
                                   const float box_top, const float box_height)
{
    auto* baked = font->GetFontBaked(size);
    float top = FLT_MAX, bottom = -FLT_MAX;
    if (baked)
    {
        for (const char* part : parts)
            for (const char* c = part; *c; ++c)
            {
                const auto* glyph = baked->FindGlyph(static_cast<ImWchar>(static_cast<unsigned char>(*c)));
                if (!glyph || !glyph->Visible)
                    continue;
                top = std::min(top, glyph->Y0);
                bottom = std::max(bottom, glyph->Y1);
            }
    }
    if (top > bottom)
    {
        top = 0.0f;
        bottom = size;
    }
    return box_top + (box_height - (bottom - top)) * 0.5f - top;
}

struct watermark_chip
{
    stat_icon icon{};
    bool no_icon{};
    char value[24]{};
    ImU32 value_color{};
    ImU32 icon_color{};
};

struct watermark_chips
{
    std::array<watermark_chip, 4> items{};
    int count{};

    [[nodiscard]] std::span<const watermark_chip> view() const
    {
        return {items.data(), static_cast<std::size_t>(count)};
    }
};

[[nodiscard]] watermark_chips collect_chips(const config::general_profile::watermark& cfg, const int fps,
                                            const int ping, const float loss, const float cpu)
{
    watermark_chips chips{};
    if (cfg.show_fps)
    {
        auto& c = chips.items[chips.count++];
        c.no_icon = true;
        std::snprintf(c.value, sizeof(c.value), "%d FPS", fps);
        c.value_color = k_wm_text;
    }
    if (cfg.show_ping)
    {
        auto& c = chips.items[chips.count++];
        c.icon = stat_icon::ping;
        std::snprintf(c.value, sizeof(c.value), "%dms", ping);
        c.value_color = threshold_color(static_cast<float>(ping), 80.0f, 150.0f);
        c.icon_color = k_wm_muted;
    }
    if (cfg.show_loss)
    {
        auto& c = chips.items[chips.count++];
        c.icon = stat_icon::loss;
        std::snprintf(c.value, sizeof(c.value), "%.0f%%", loss);
        c.value_color = threshold_color(loss, 2.0f, 15.0f);
        c.icon_color = loss_icon_color(loss, 2.0f, 15.0f);
    }
    if (cfg.show_cpu)
    {
        auto& c = chips.items[chips.count++];
        c.icon = stat_icon::cpu;
        std::snprintf(c.value, sizeof(c.value), "%.1f%%", cpu);
        c.value_color = threshold_color(cpu, 8.0f, 25.0f);
        c.icon_color = k_wm_muted;
    }
    return chips;
}

// Fonts and spacing of the watermark at the current scale, and the panel size in both orientations.
struct watermark_metrics
{
    ImFont* brand_font{};
    float brand_size{};
    ImFont* text_font{};
    float text_size{};
    float scale{}; // DPI times the user's scale
    // Every metric reserves the same square em-box, which keeps the glyph/value
    // rhythm identical in both horizontal and vertical watermark layouts.
    float icon_size{};
    float brand_width{};
    float label_gap{};
    float h_pad_left{};
    float h_gap{};
    float divider_gap{};
    float v_pad_x{};
    float v_pad_y{};
    float v_brand_gap{};
    float v_line_gap{};
    float v_inner_width{};
    ImVec2 horizontal_size{};
    ImVec2 vertical_size{};

    [[nodiscard]] float chip_width(const watermark_chip& c) const
    {
        const auto value = text_width(text_font, text_size, c.value);
        return c.no_icon ? value : icon_size + label_gap + value;
    }
};

// "VESTA" uses the menu's brand font (as the sidebar logo) at a much smaller size; the stats use its regular font.
[[nodiscard]] watermark_metrics measure_watermark(const float dpi_scale, const float user_scale,
                                                  const watermark_chips& chips)
{
    watermark_metrics m{};
    m.scale = dpi_scale * user_scale;
    m.brand_font = resolve_font(render::fonts().menu_brand_30, 1.0f).font;
    m.brand_size = 22.5f * m.scale;
    const auto text_style = resolve_font(render::fonts().menu_regular_12, user_scale);
    m.text_font = text_style.font;
    m.text_size = text_style.size;
    m.icon_size = m.text_size;
    m.brand_width = text_width(m.brand_font, m.brand_size, k_wm_brand);
    m.label_gap = 6.0f * m.scale;

    m.h_pad_left = 26.0f * m.scale;
    const auto h_pad_right = 28.0f * m.scale;
    const auto h_pad_y = 5.0f * m.scale;
    m.h_gap = 14.0f * m.scale;
    m.divider_gap = 14.0f * m.scale;
    auto h_content = m.brand_width;
    if (chips.count > 0)
        h_content += m.divider_gap * 2.0f + 1.0f;
    for (int i = 0; i < chips.count; ++i)
        h_content += (i > 0 ? m.h_gap : 0.0f) + m.chip_width(chips.items[i]);
    m.horizontal_size = {h_content + m.h_pad_left + h_pad_right, std::max(m.brand_size, m.text_size) + h_pad_y * 2.0f};

    m.v_pad_x = 18.0f * m.scale;
    m.v_pad_y = 12.0f * m.scale;
    m.v_brand_gap = 10.0f * m.scale;
    m.v_line_gap = 7.0f * m.scale;
    m.v_inner_width = m.brand_width;
    for (const auto& chip : chips.view())
        m.v_inner_width = std::max(m.v_inner_width, m.chip_width(chip));
    auto v_content = m.brand_size;
    if (chips.count > 0)
        v_content += m.v_brand_gap + chips.count * m.text_size + (chips.count - 1) * m.v_line_gap;
    m.vertical_size = {m.v_inner_width + m.v_pad_x * 2.0f, v_content + m.v_pad_y * 2.0f};
    return m;
}

struct watermark_placement
{
    ImVec2 min{};
    ImVec2 size{};
    bool vertical{};
};

// Positions the panel from its saved layout or the drag in progress. Dragged against a side edge the watermark turns
// vertical, against the top or bottom horizontal; its size eases towards the target, so toggling a stat or turning the
// panel grows it smoothly.
[[nodiscard]] watermark_placement place_watermark(config::general_profile::watermark& cfg, const watermark_metrics& m,
                                                  panel_drag& drag, ImVec2& animated_size, const bool interactive,
                                                  const float dpi_scale)
{
    const screen_ui_space ui_space{};
    const auto display = ui_space.display();
    const auto position_scale = ui_space.position_scale();
    const float snap = 22.0f * dpi_scale;

    bool vertical = cfg.vertical;
    const auto initial_size = vertical ? m.vertical_size : m.horizontal_size;
    migrate_screen_layout(cfg.layout, initial_size, display, dpi_scale, position_scale);
    auto position = resolve_screen_layout(cfg.layout, initial_size, display, dpi_scale, position_scale);
    position = drag.update(interactive, position, initial_size, &cfg.layout.scale);
    if (drag.active())
    {
        const auto center = position + initial_size * 0.5f;
        const auto near_side = position.x <= snap || position.x + initial_size.x >= display.x - snap;
        const auto near_top_or_bottom = position.y <= snap || position.y + initial_size.y >= display.y - snap;
        const auto side_middle = center.y >= display.y * 0.25f && center.y <= display.y * 0.75f;
        const auto horizontal_middle = center.x >= display.x * 0.25f && center.x <= display.x * 0.75f;
        if (!vertical && near_side && side_middle)
            vertical = true;
        else if (vertical && near_top_or_bottom && horizontal_middle)
            vertical = false;
    }

    const auto target = vertical ? m.vertical_size : m.horizontal_size;
    if (animated_size.x <= 0.0f)
        animated_size = target;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    const float k = 1.0f - std::pow(0.0025f, dt);
    animated_size += (target - animated_size) * k;

    if (!drag.active())
        position = resolve_screen_layout(cfg.layout, animated_size, display, dpi_scale, position_scale);
    const ImVec2 min{std::clamp(position.x, 0.0f, std::max(0.0f, display.x - animated_size.x)),
                     std::clamp(position.y, 0.0f, std::max(0.0f, display.y - animated_size.y))};
    if (drag.active())
        capture_screen_layout(cfg.layout, min, animated_size, display, dpi_scale, position_scale);
    cfg.vertical = vertical;
    return {min, animated_size, vertical};
}

void draw_watermark_vertical(ImDrawList* draw, const watermark_metrics& m, const watermark_chips& chips,
                             const ImVec2 min, const ImVec2 max)
{
    const auto width = max.x - min.x;
    float y = min.y + m.v_pad_y;
    draw->AddText(m.brand_font, m.brand_size,
                  {min.x + (width - m.brand_width) * 0.5f,
                   ink_centered_y(m.brand_font, m.brand_size, {k_wm_brand}, y, m.brand_size)},
                  k_wm_text, k_wm_brand);
    y += m.brand_size;
    if (chips.count == 0)
        return;

    y += m.v_brand_gap * 0.5f;
    draw->AddLine({min.x + m.v_pad_x, y}, {max.x - m.v_pad_x, y}, k_wm_border, std::max(1.0f, m.scale));
    y += m.v_brand_gap * 0.5f;

    const auto block_x = min.x + (width - m.v_inner_width) * 0.5f;
    for (const auto& c : chips.view())
    {
        float x = block_x;
        const auto text_y = ink_centered_y(m.text_font, m.text_size, {c.value}, y, m.text_size);
        if (!c.no_icon)
        {
            draw_stat_icon(draw, c.icon, x, y + (m.text_size - m.icon_size) * 0.5f, m.icon_size, c.icon_color);
            x += m.icon_size + m.label_gap;
        }
        draw->AddText(m.text_font, m.text_size, {x, text_y}, c.value_color, c.value);
        y += m.text_size + m.v_line_gap;
    }
}

void draw_watermark_horizontal(ImDrawList* draw, const watermark_metrics& m, const watermark_chips& chips,
                               const ImVec2 min, const ImVec2 max)
{
    const auto height = max.y - min.y;
    const float y_offset = 2.0f * m.scale;
    auto cursor_x = min.x + m.h_pad_left;
    draw->AddText(m.brand_font, m.brand_size,
                  {cursor_x, ink_centered_y(m.brand_font, m.brand_size, {k_wm_brand}, min.y, height) + y_offset},
                  k_wm_text, k_wm_brand);
    cursor_x += m.brand_width;

    if (chips.count > 0)
    {
        cursor_x += m.divider_gap;
        draw->AddLine({cursor_x, min.y + 6.0f * m.scale}, {cursor_x, max.y - 6.0f * m.scale}, k_wm_border,
                      std::max(1.0f, m.scale));
        cursor_x += m.divider_gap;
    }

    for (int i = 0; i < chips.count; ++i)
    {
        if (i > 0)
            cursor_x += m.h_gap;
        const auto& c = chips.items[i];
        const auto text_y = ink_centered_y(m.text_font, m.text_size, {c.value}, min.y, height) + y_offset;
        if (!c.no_icon)
        {
            draw_stat_icon(draw, c.icon, cursor_x, min.y + (height - m.icon_size) * 0.5f + y_offset, m.icon_size,
                           c.icon_color);
            cursor_x += m.icon_size + m.label_gap;
        }
        draw->AddText(m.text_font, m.text_size, {cursor_x, text_y}, c.value_color, c.value);
        cursor_x += text_width(m.text_font, m.text_size, c.value);
    }
}

} // namespace

struct panels::state : hud_state
{
};

panels::panels()
    : m_state(std::make_unique<state>())
{
}

panels::~panels() = default;

void panels::draw_movement_info(const bool interactive)
{
    auto& settings = config::general_settings;
    auto& cfg = settings.m_movement_info;
    // Movement telemetry is configured from the Bunny Hop popup, so the popup's
    // master switch also controls whether this panel is shown.
    if (!cfg.enabled && !settings.m_bunny_hop.enabled)
        return;

    const auto pawn = game::local_player().pawn();
    const bool alive = pawn && game::local_player().alive();
    foundation::vec3 velocity{};
    std::uint32_t flags{};
    std::uint32_t ground_entity{};
    bool valid{};
    if (alive)
    {
        valid = game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id),
                                                   &velocity, sizeof(velocity));
        valid = game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id), &flags,
                                                   sizeof(flags)) && valid;
        valid = game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_hGroundEntity"_id),
                                                   &ground_entity, sizeof(ground_entity)) && valid;
    }
    if (!valid && !interactive)
        return;

    const bool on_ground = (flags & 1u) != 0u || ground_entity != 0xffffffffu;
    std::vector<compact_panel_row> rows;
    if (cfg.show_speed)
        rows.push_back({std::format("Speed: {:.0f} u/s", velocity.length_2d()), k_wm_text});
    if (cfg.show_ground)
        rows.push_back({on_ground ? "Grounded" : "Airborne",
                        on_ground ? IM_COL32(110, 225, 135, 255) : IM_COL32(238, 68, 68, 255)});
    if (cfg.show_velocity)
    {
        rows.push_back({std::format("Velocity X: {:.0f}", velocity.x), k_wm_muted});
        rows.push_back({std::format("Velocity Y: {:.0f}", velocity.y), k_wm_muted});
        rows.push_back({std::format("Velocity Z: {:.0f}", velocity.z), k_wm_muted});
    }
    if (rows.empty())
        rows.push_back({"No movement data", k_wm_muted});
    draw_compact_panel("Movement", rows, cfg.layout, m_state->movement, interactive);
}

void panels::draw_watermark(const bool interactive)
{
    auto& cfg = config::general_settings.m_watermark;
    if (!cfg.enabled)
        return;

    auto& state = *m_state;
    const auto fps = state.frame_rate.tick();
    const auto ping = state.ping.sample();
    const auto loss = state.instability.sample(ping);
    const auto cpu = state.cpu.sample();
    const auto chips = collect_chips(cfg, fps, ping, loss, cpu);

    const auto dpi_scale = overlay_dpi_scale();
    cfg.layout.scale = std::clamp(cfg.layout.scale, 0.55f, 2.0f);
    const auto metrics = measure_watermark(dpi_scale, cfg.layout.scale, chips);
    const auto placement = place_watermark(cfg, metrics, state.watermark, state.watermark_size, interactive, dpi_scale);

    const auto min = placement.min;
    const auto max = placement.min + placement.size;
    auto* draw = ImGui::GetForegroundDrawList();
    const float rounding =
        placement.vertical ? 14.0f * metrics.scale : std::min(placement.size.x, placement.size.y) * 0.5f;
    draw_panel_shadow(draw, min, max, rounding, 1.6f * metrics.scale, 30.0f);
    draw->AddRectFilled(min, max, k_wm_bg, rounding);
    draw->AddRect(min, max, k_wm_border, rounding, ImDrawFlags_None, std::max(1.0f, metrics.scale));

    // Content is clipped to the animated panel so it reveals/hides cleanly while
    // the box grows or shrinks.
    draw->PushClipRect(min, max, true);
    if (placement.vertical)
        draw_watermark_vertical(draw, metrics, chips, min, max);
    else
        draw_watermark_horizontal(draw, metrics, chips, min, max);
    draw->PopClipRect();
}

void panels::draw_spectator_list(bool interactive)
{
    auto& cfg = config::general_settings.m_spectator_list;
    if (!cfg.enabled)
    {
        return;
    }

    // The 200ms observer-chain scan is collected on the game worker. Rendering
    // consumes an immutable snapshot and performs no process-memory calls.
    const auto specs = game::world().spectators();

    const auto dpi_scale = overlay_dpi_scale();
    cfg.layout.scale = std::clamp(cfg.layout.scale, 0.55f, 2.0f);
    const auto user_scale = cfg.layout.scale;
    const auto metric_scale = dpi_scale * user_scale;
    const auto title_style = resolve_font(render::fonts().menu_semibold_13, user_scale);
    auto* title_font = title_style.font;
    const auto title_size = title_style.size;
    const auto text_style = resolve_font(render::fonts().menu_regular_12, user_scale);
    auto* text_font = text_style.font;
    const auto text_size = text_style.size;

    const auto measure = [](ImFont* font, float size, const char* text)
    { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x; };

    char header[48];
    std::snprintf(header, sizeof(header), "%s (%d)", render::localization::tr("Spectators"),
                  static_cast<int>(specs->size()));

    const float pad_x = 14.0f * metric_scale;
    const float pad_y = 10.0f * metric_scale;
    const float row_gap = 5.0f * metric_scale;
    const float title_gap = 8.0f * metric_scale;
    const float row_height = text_size;
    float inner_w = measure(title_font, title_size, header);
    for (const auto& s : *specs)
    {
        const float w = measure(text_font, text_size, s.name.c_str());
        inner_w = std::max(inner_w, w);
    }
    inner_w = std::max(inner_w, 120.0f * metric_scale);

    const int row_count = static_cast<int>(specs->size());
    const float panel_w = inner_w + pad_x * 2.0f;
    const float panel_h = pad_y * 2.0f + title_size +
                          (row_count > 0 ? title_gap + row_count * row_height + (row_count - 1) * row_gap : 0.0f);

    const screen_ui_space ui_space{};
    const auto display = ui_space.display();
    const auto position_scale = ui_space.position_scale();
    const ImVec2 panel_size{panel_w, panel_h};
    migrate_screen_layout(cfg.layout, panel_size, display, dpi_scale, position_scale);
    auto position = resolve_screen_layout(cfg.layout, panel_size, display, dpi_scale, position_scale);
    position = clamp_to_display(m_state->spectators.update(interactive, position, panel_size, &cfg.layout.scale),
                                panel_size, display);
    if (m_state->spectators.active())
        capture_screen_layout(cfg.layout, position, panel_size, display, dpi_scale, position_scale);

    const ImVec2 draw_min = position;
    const ImVec2 draw_max{position.x + panel_w, position.y + panel_h};

    auto* draw = ImGui::GetForegroundDrawList();
    const float rounding = 8.0f * metric_scale;
    draw_panel_shadow(draw, draw_min, draw_max, rounding, 1.4f * metric_scale, 28.0f);
    draw->AddRectFilled(draw_min, draw_max, k_wm_bg, rounding);
    draw->AddRect(draw_min, draw_max, k_wm_border, rounding, ImDrawFlags_None, std::max(1.0f, metric_scale));

    draw->PushClipRect(draw_min, draw_max, true);
    float y = draw_min.y + pad_y;
    draw->AddText(title_font, title_size, {draw_min.x + pad_x, y}, k_wm_text, header);
    y += title_size + title_gap;
    for (const auto& s : *specs)
    {
        const auto name_x = draw_min.x + pad_x;
        const auto text_y = y + (row_height - text_size) * 0.5f;
        draw->AddText(text_font, text_size, {name_x, text_y}, k_wm_text, s.name.c_str());
        y += row_height + row_gap;
    }
    draw->PopClipRect();
}

namespace
{
void draw_compact_panel(const char* title, const std::vector<compact_panel_row>& rows,
                        config::general_profile::screen_layout& layout, panel_drag& drag, const bool interactive)
{
    if (rows.empty() && !interactive)
        return;
    const auto dpi = overlay_dpi_scale();
    layout.scale = std::clamp(layout.scale, 0.55f, 2.0f);
    const auto metric = dpi * layout.scale;
    const auto title_style = resolve_font(render::fonts().menu_semibold_13, layout.scale);
    auto* title_font = title_style.font;
    const auto title_size = title_style.size;
    const auto text_style = resolve_font(render::fonts().menu_regular_12, layout.scale);
    auto* text_font = text_style.font;
    const auto text_size = text_style.size;
    const auto width_of = [](ImFont* font, float size, const char* value)
    { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, value).x; };
    const auto pad_x = 14.0f * metric, pad_y = 10.0f * metric;
    const auto gap = 5.0f * metric, title_gap = 8.0f * metric;
    auto inner_w = std::max(132.0f * metric, width_of(title_font, title_size, title));
    for (const auto& row : rows)
        inner_w = std::max(inner_w, width_of(text_font, text_size, row.text.c_str()));
    const auto row_count = std::max<std::size_t>(rows.size(), interactive ? 1 : 0);
    const ImVec2 panel_size{inner_w + pad_x * 2.0f, pad_y * 2.0f + title_size + title_gap + row_count * text_size +
                                                        (row_count ? row_count - 1 : 0) * gap};
    const screen_ui_space ui_space{};
    const auto display = ui_space.display(), position_scale = ui_space.position_scale();
    migrate_screen_layout(layout, panel_size, display, dpi, position_scale);
    auto position = resolve_screen_layout(layout, panel_size, display, dpi, position_scale);
    position = clamp_to_display(drag.update(interactive, position, panel_size), panel_size, display);
    if (drag.active())
        capture_screen_layout(layout, position, panel_size, display, dpi, position_scale);
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 maximum{position.x + panel_size.x, position.y + panel_size.y};
    const auto rounding = 12.0f * metric;
    draw->AddRectFilled(position, maximum, k_wm_bg, rounding);
    draw->AddRect(position, maximum, k_wm_border, rounding, ImDrawFlags_None, std::max(1.0f, metric));
    auto y = position.y + pad_y;
    draw->AddText(title_font, title_size, {position.x + pad_x, y}, k_wm_text, title);
    y += title_size + title_gap;
    if (rows.empty())
        draw->AddText(text_font, text_size, {position.x + pad_x, y}, k_wm_muted,
                      render::localization::tr("No active entries"));
    else
        for (const auto& row : rows)
        {
            draw->AddText(text_font, text_size, {position.x + pad_x, y}, row.color, row.text.c_str());
            y += text_size + gap;
        }
}
} // namespace

void panels::draw_event_log(const bool interactive)
{
    auto& cfg = config::general_settings.m_event_log;
    if (!cfg.enabled)
        return;
    const auto maximum = std::clamp(cfg.max_entries, 1, 5);
    auto entries = features::visuals::event_log().snapshot(cfg.duration, maximum);
    if (entries.empty() && !interactive)
        return;
    const auto now = std::chrono::steady_clock::now();
    const auto dpi = overlay_dpi_scale();
    cfg.layout.scale = std::clamp(cfg.layout.scale, 0.55f, 2.0f);
    const auto metric = dpi * cfg.layout.scale;
    const auto style = resolve_font(render::fonts().menu_regular_12, cfg.layout.scale);
    auto* font = style.font;
    const auto font_size = style.size;
    const auto row_height = std::max(30.0f * metric, font_size + 16.0f * metric);
    const auto gap = 6.0f * metric;
    auto width = 172.0f * metric;
    for (const auto& entry : entries)
        width = std::max(width, font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, entry.text.c_str()).x + 30.0f * metric);
    const auto visible_rows = std::max<std::size_t>(entries.size(), interactive ? 1 : 0);
    const ImVec2 bounds{width, visible_rows * row_height + (visible_rows ? visible_rows - 1 : 0) * gap};
    const screen_ui_space ui_space{};
    const auto display = ui_space.display(), position_scale = ui_space.position_scale();
    migrate_screen_layout(cfg.layout, bounds, display, dpi, position_scale);
    auto anchor = resolve_screen_layout(cfg.layout, bounds, display, dpi, position_scale);
    anchor = clamp_to_display(m_state->events.update(interactive, anchor, bounds), bounds, display);
    if (m_state->events.active())
        capture_screen_layout(cfg.layout, anchor, bounds, display, dpi, position_scale);
    auto* draw = ImGui::GetForegroundDrawList();
    if (entries.empty())
    {
        const ImVec2 maximum_pos{anchor.x + width, anchor.y + row_height};
        draw->AddRectFilled(anchor, maximum_pos, k_wm_bg, 9.0f * metric);
        draw->AddRect(anchor, maximum_pos, k_wm_border, 9.0f * metric);
        draw->AddText(font, font_size, anchor + ImVec2{15.0f * metric, (row_height - font_size) * 0.5f}, k_wm_muted,
                      render::localization::tr("No active entries"));
        return;
    }
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries[i];
        const auto age = std::chrono::duration<float>(now - entry.timestamp).count();
        const auto lifetime = std::max(0.5f, cfg.duration);
        const auto fade_in = std::clamp(age / 0.16f, 0.0f, 1.0f);
        const auto fade_out = std::clamp((lifetime - age) / 0.55f, 0.0f, 1.0f);
        const auto alpha = fade_in * fade_in * (3.0f - 2.0f * fade_in) * fade_out * fade_out * (3.0f - 2.0f * fade_out);
        const auto rise = std::clamp(age / lifetime, 0.0f, 1.0f) * 8.0f * metric;
        const auto slide = (1.0f - fade_in) * 18.0f * metric;
        const ImVec2 minimum{anchor.x + slide, anchor.y + static_cast<float>(i) * (row_height + gap) - rise};
        const ImVec2 maximum_pos{minimum.x + width, minimum.y + row_height};
        const auto accent = entry.kind == features::visuals::event_kind::kill      ? IM_COL32(255, 112, 135, 255)
                            : entry.kind == features::visuals::event_kind::hit     ? IM_COL32(120, 215, 255, 255)
                            : entry.kind == features::visuals::event_kind::blocked ? IM_COL32(245, 190, 95, 255)
                                                                                   : IM_COL32(180, 188, 215, 255);
        draw->AddRectFilled(minimum, maximum_pos, with_alpha(k_wm_bg, alpha), 9.0f * metric);
        draw->AddRect(minimum, maximum_pos, with_alpha(k_wm_border, alpha), 9.0f * metric, ImDrawFlags_None,
                      std::max(1.0f, metric));
        draw->AddRectFilled(minimum, {minimum.x + 3.0f * metric, maximum_pos.y}, with_alpha(accent, alpha),
                            9.0f * metric, ImDrawFlags_RoundCornersLeft);
        draw->AddText(font, font_size, minimum + ImVec2{15.0f * metric, (row_height - font_size) * 0.5f},
                      with_alpha(k_wm_text, alpha), entry.text.c_str());
    }
}


// A system message plate in the top-left corner: always the same place and look, not configurable. It reports
// what the player needs to know right now (map geometry missing, compatibility mode, Lua unavailable, previous
// crash). Each plate can be closed with its cross while the menu is open; the overlay ignores the mouse otherwise.
void panels::draw_system_notices(const bool interactive)
{
    using diagnostics::readiness::state;
    auto& closed = m_state->closed_notices;
    auto& loading_serial = m_state->loading_serial;
    auto& loading_since = m_state->loading_since;
    const auto now = std::chrono::steady_clock::now();
    auto& health = diagnostics::health();

    ui::notices::inputs input;
    input.geometry = health.geometry.snapshot();
    input.overlay = health.overlay.snapshot();
    input.scripting = health.scripting.snapshot();
    input.previous_session_crashed = health.previous_session_crashed.load(std::memory_order_acquire);
    input.input_hook_failed = platform::windows::bound_input().hook_failed();
    if (input.geometry.value == state::loading)
    {
        if (loading_serial != input.geometry.serial)
        {
            loading_serial = input.geometry.serial;
            loading_since = now;
        }
        input.geometry_loading_seconds = std::chrono::duration<double>(now - loading_since).count();
    }
    else
        loading_serial = 0;

    const auto shown = closed.visible(ui::notices::collect(input));
    if (shown.empty())
        return;

    const auto metric = overlay_dpi_scale();
    const auto style = resolve_font(render::fonts().menu_regular_12, 1.1f);
    auto* font = style.font;
    const auto text_size = style.size;
    const auto title_size = text_size * 1.05f;
    const float width = 380.0f * metric, pad = 14.0f * metric, icon = 22.0f * metric, gap = 8.0f * metric;
    const float text_width = width - pad * 2.0f - 4.0f * metric - 24.0f * metric;
    const auto mouse = physical_mouse_position();
    const bool clicked = interactive && ImGui::IsMouseClicked(0);
    auto* draw = ImGui::GetForegroundDrawList();
    float y = 18.0f * metric;
    const float x = 18.0f * metric;
    const ui::notices::notice* to_close = nullptr;

    for (const auto& item : shown)
    {
        const auto* text = render::localization::tr(item.text);
        const auto measured = font->CalcTextSizeA(text_size, FLT_MAX, text_width, text);
        const float height = pad * 2.0f + title_size + 4.0f * metric + measured.y +
                             (interactive ? 0.0f : text_size * 0.9f + 4.0f * metric);
        const ImVec2 minimum{x, y}, maximum{x + width, y + height};
        const ImU32 accent = item.severity == ui::notices::level::error     ? IM_COL32(255, 112, 135, 255)
                             : item.severity == ui::notices::level::warning ? IM_COL32(245, 190, 95, 255)
                                                                            : IM_COL32(167, 139, 250, 255);
        const char* title = item.severity == ui::notices::level::error     ? "Error"
                            : item.severity == ui::notices::level::warning ? "Warning"
                                                                           : "Notice";
        // Same panel language as the event log and watermark: translucent dark body, hairline border, 3 px accent bar.
        const float rounding = 9.0f * metric;
        draw->AddRectFilled(minimum, maximum, k_wm_bg_solid, rounding);
        draw->AddRect(minimum, maximum, k_wm_border, rounding, ImDrawFlags_None, std::max(1.0f, metric));
        draw->AddRectFilled(minimum, {minimum.x + 3.0f * metric, maximum.y}, accent, rounding,
                            ImDrawFlags_RoundCornersLeft);
        const float text_x = minimum.x + pad + 4.0f * metric;
        draw->AddText(font, title_size, {text_x, minimum.y + pad}, accent, render::localization::tr(title));
        draw->AddText(font, text_size, {text_x, minimum.y + pad + title_size + 4.0f * metric}, k_wm_text, text, nullptr,
                      text_width);
        if (!interactive)
            draw->AddText(font, text_size * 0.9f, {text_x, maximum.y - pad - text_size * 0.9f}, k_wm_muted,
                          render::localization::tr("Open the menu to close this message."));

        // Close cross.
        const float cross = 9.0f * metric;
        const ImVec2 cross_center{maximum.x - pad - cross * 0.5f, minimum.y + pad + icon * 0.5f};
        const bool hovered = interactive && mouse.x >= cross_center.x - cross && mouse.x <= cross_center.x + cross &&
                             mouse.y >= cross_center.y - cross && mouse.y <= cross_center.y + cross;
        const ImU32 cross_color = interactive ? (hovered ? k_wm_text : k_wm_muted) : with_alpha(k_wm_muted, 0.45f);
        const float arm = cross * 0.5f;
        draw->AddLine({cross_center.x - arm, cross_center.y - arm}, {cross_center.x + arm, cross_center.y + arm},
                      cross_color, std::max(1.5f, 1.6f * metric));
        draw->AddLine({cross_center.x - arm, cross_center.y + arm}, {cross_center.x + arm, cross_center.y - arm},
                      cross_color, std::max(1.5f, 1.6f * metric));
        if (hovered && clicked)
            to_close = &item;
        y = maximum.y + gap;
    }
    if (to_close)
        closed.close(*to_close);
}

void panels::draw_keybind_list(const bool interactive)
{
    auto& cfg = config::general_settings.m_keybind_list;
    if (!cfg.enabled)
        return;
    std::vector<compact_panel_row> rows{};
    const auto combat = config::combat_settings.get(game::local_player().weapon_type());
    const auto mode_visible = [&](const int mode)
    {
        return mode == config::combat_profile::activation::always   ? cfg.show_always
               : mode == config::combat_profile::activation::toggle ? cfg.show_toggle
                                                                    : cfg.show_hold;
    };
    const auto add_combat = [&](std::string name, const bool enabled, const int mode, const int key)
    {
        if (!enabled || !mode_visible(mode) || !config::combat_profile::activation_active(mode, key))
            return;
        const auto* mode_name = mode == config::combat_profile::activation::always   ? "always"
                                : mode == config::combat_profile::activation::toggle ? "toggle"
                                                                                     : "hold";
        rows.push_back({std::format("{}  [{}]", std::move(name), mode_name), k_wm_text});
    };
    add_combat("Aimbot", combat.aimbot.enabled, combat.aimbot.activation_mode, combat.aimbot.key);
    add_combat("Trigger", combat.triggerbot.enabled, combat.triggerbot.activation_mode, combat.triggerbot.key);
    const auto& global = config::combat_settings.global;
    add_combat(std::format("Aim Damage: {}", static_cast<int>(std::lround(global.aimbot_min_damage_override))),
               global.aimbot_enabled && global.aimbot_min_damage_override_enabled,
               global.aimbot_min_damage_override_mode, global.aimbot_min_damage_override_key);
    add_combat(std::format("Trigger Damage: {}", static_cast<int>(std::lround(global.triggerbot_min_damage_override))),
               global.triggerbot_enabled && global.triggerbot_min_damage_override_enabled,
               global.triggerbot_min_damage_override_mode, global.triggerbot_min_damage_override_key);
    const auto& player_esp = config::visual_settings.m_player;
    const auto visual_mode_visible = [&](const int mode)
    {
        return mode == config::visual_profile::player::always_on ? cfg.show_always
               : mode == config::visual_profile::player::toggle  ? cfg.show_toggle
                                                                 : cfg.show_hold;
    };
    if (visual_mode_visible(player_esp.activation_mode) && player_esp.active())
    {
        const auto* mode = player_esp.activation_mode == config::visual_profile::player::always_on ? "always"
                           : player_esp.activation_mode == config::visual_profile::player::toggle  ? "toggle"
                                                                                                   : "hold";
        rows.push_back({std::format("Player ESP  [{}]", mode), k_wm_text});
    }
    const auto& radar = config::visual_settings.m_radar;
    if (visual_mode_visible(radar.activation_mode) && radar.active())
    {
        const auto* mode = radar.activation_mode == config::visual_profile::radar::always_on ? "always"
                           : radar.activation_mode == config::visual_profile::radar::toggle  ? "toggle"
                                                                                             : "hold";
        rows.push_back({std::format("Radar  [{}]", mode), k_wm_text});
    }
    const auto key_down = [](int key) { return key > 0 && (::GetAsyncKeyState(key) & 0x8000) != 0; };
    const auto& misc = config::general_settings;
    if (cfg.show_hold && global.grenade_aim.enabled && key_down(global.grenade_aim.key))
        rows.push_back({"Grenade Aim  [hold]", k_wm_text});
    if (cfg.show_hold && misc.m_bunny_hop.enabled && key_down(misc.m_bunny_hop.activation_key))
        rows.push_back({"Bunny Hop  [hold]", k_wm_text});
    if (cfg.show_hold && misc.m_edge_jump.enabled && key_down(misc.m_edge_jump.activation_key))
        rows.push_back({"Edge Jump  [hold]", k_wm_text});
    if (cfg.show_hold && misc.m_nade_helper.enabled && misc.m_nade_helper.aim_assist &&
        key_down(misc.m_nade_helper.aim_key))
        rows.push_back({"Nade Helper  [hold]", k_wm_text});
    if (cfg.show_hold && misc.m_auto_stop.enabled && features::misc::auto_stop().active())
        rows.push_back({"Auto Stop  [active]", k_wm_text});
    if (cfg.show_always && config::visual_settings.m_no_flash.enabled)
        rows.push_back({"No Flash  [always]", k_wm_text});
    if (cfg.show_always && config::visual_settings.m_no_smoke.enabled)
        rows.push_back({"No Smoke  [always]", k_wm_text});
    draw_compact_panel(render::localization::tr("Active Binds"), rows, cfg.layout, m_state->keybinds, interactive);
}

void panels::draw_bomb_info(bool interactive)
{
    auto& cfg = config::visual_settings.m_bomb;
    auto& layout = config::general_settings.m_bomb_info.layout;
    if (!cfg.enabled || !cfg.show_info_panel)
        return;

    auto state = features::visuals::bomb().info_snapshot();
    if (!state.planted)
    {
        if (!interactive)
            return;
        // Menu-open preview makes the otherwise event-driven panel positionable.
        state.planted = true;
        state.time_remaining = 32.8f;
        state.timer_length = 40.0f;
        state.bomb_site = 0;
        state.predicted_damage = 124;
        state.local_health = 100;
    }

    const auto dpi_scale = overlay_dpi_scale();
    layout.scale = std::clamp(layout.scale, 0.55f, 2.0f);
    const auto user_scale = layout.scale;
    const auto metric_scale = dpi_scale * user_scale;
    const auto& fonts = render::fonts();
    const auto label = resolve_font(fonts.menu_semibold_13, user_scale);
    auto* title_font = label.font;
    auto* text_font = label.font;
    const auto title_size = label.size;
    const auto text_size = label.size;
    auto* icon_font = resolve_font(fonts.weapons_15, 1.0f).font;
    const auto icon_size = 18.0f * user_scale;
    const ImVec2 panel_size{238.0f * metric_scale, 86.0f * metric_scale};

    const screen_ui_space ui_space{};
    const auto display = ui_space.display();
    const auto position_scale = ui_space.position_scale();
    migrate_screen_layout(layout, panel_size, display, dpi_scale, position_scale);
    auto position = resolve_screen_layout(layout, panel_size, display, dpi_scale, position_scale);
    position =
        clamp_to_display(m_state->bomb.update(interactive, position, panel_size, &layout.scale), panel_size, display);
    if (m_state->bomb.active())
        capture_screen_layout(layout, position, panel_size, display, dpi_scale, position_scale);

    const ImVec2 draw_min = position;
    const ImVec2 draw_max{position.x + panel_size.x, position.y + panel_size.y};
    auto* draw = ImGui::GetForegroundDrawList();
    const auto rounding = 12.0f * metric_scale;
    const auto background = IM_COL32(cfg.panel_background.r, cfg.panel_background.g, cfg.panel_background.b,
                                     std::min<int>(cfg.panel_background.a, 150));
    draw_panel_shadow(draw, draw_min, draw_max, rounding, 1.4f * metric_scale, 28.0f);
    draw->AddRectFilled(draw_min, draw_max, background, rounding);
    draw->AddRect(draw_min, draw_max, k_wm_border, rounding, ImDrawFlags_None, std::max(1.0f, metric_scale));

    const auto bomb_color = cfg.bomb_color_t;
    const auto bomb_u32 = IM_COL32(bomb_color.r, bomb_color.g, bomb_color.b, bomb_color.a);
    const auto danger_amount = std::clamp(1.0f - state.time_remaining / 10.0f, 0.0f, 1.0f);
    const auto mix = [danger_amount](std::uint8_t from, std::uint8_t to)
    { return static_cast<int>(from + (to - from) * danger_amount); };
    const auto timer_color = IM_COL32(mix(cfg.timer_text_color.r, 238), mix(cfg.timer_text_color.g, 68),
                                      mix(cfg.timer_text_color.b, 68), cfg.timer_text_color.a);

    draw->PushClipRect(draw_min, draw_max, true);
    const auto center = ImVec2{draw_min.x + 43.0f * metric_scale, draw_min.y + panel_size.y * 0.5f};
    const auto bomb_fraction = std::clamp(state.time_remaining / std::max(1.0f, state.timer_length), 0.0f, 1.0f);
    const auto defuse_fraction =
        state.being_defused ? std::clamp(state.defuse_remaining / std::max(0.1f, state.defuse_length), 0.0f, 1.0f)
                            : 0.0f;
    const auto defuse_color = state.defuse_success ? IM_COL32(cfg.bomb_color_ct.r, cfg.bomb_color_ct.g,
                                                              cfg.bomb_color_ct.b, cfg.bomb_color_ct.a)
                                                   : IM_COL32(255, 92, 62, 255);
    const auto bomb_radius = 21.0f * metric_scale;
    const auto ring_width = std::max(1.6f, 2.2f * metric_scale);
    constexpr auto start = -std::numbers::pi_v<float> * 0.5f;
    draw->AddCircle(center, bomb_radius, IM_COL32(0, 0, 0, 145), 48, ring_width);
    if (bomb_fraction > 0.002f)
    {
        draw->PathArcTo(center, bomb_radius, start, start + 2.0f * std::numbers::pi_v<float> * bomb_fraction, 48);
        draw->PathStroke(timer_color, 0, ring_width);
    }
    if (state.being_defused && defuse_fraction > 0.002f)
    {
        draw->PathArcTo(center, bomb_radius, start, start + 2.0f * std::numbers::pi_v<float> * defuse_fraction, 48);
        draw->PathStroke(defuse_color, 0, ring_width * 1.35f);
    }

    const auto* icon = state.being_defused ? "r" : "o";
    const auto icon_color = state.being_defused ? defuse_color : bomb_u32;
    const auto icon_extent = icon_font->CalcTextSizeA(icon_size, FLT_MAX, 0.0f, icon);
    draw->AddText(icon_font, icon_size, {center.x - icon_extent.x * 0.5f, center.y - icon_extent.y * 0.5f}, icon_color,
                  icon);

    const auto divider_x = draw_min.x + 79.0f * metric_scale;
    draw->AddLine({divider_x, draw_min.y + 12.0f * metric_scale}, {divider_x, draw_max.y - 12.0f * metric_scale},
                  k_wm_border, std::max(1.0f, metric_scale));
    const auto text_x = divider_x + 14.0f * metric_scale;
    const auto line_height = 22.0f * metric_scale;
    const auto first_y = draw_min.y + 9.0f * metric_scale;
    const auto draw_row = [&](float y, const char* label, const std::string& value, ImU32 value_color)
    {
        draw->AddText(title_font, title_size, {text_x, y}, k_wm_muted, label);
        const auto label_width = title_font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, label).x;
        draw->AddText(text_font, text_size, {text_x + label_width + 5.0f * metric_scale, y}, value_color,
                      value.c_str());
    };

    const auto site_value = std::string(state.bomb_site == 1 ? "B" : "A");
    auto hp_value = std::string("--");
    auto hp_color = k_wm_muted;
    if (state.predicted_damage >= 0 && state.local_health > 0)
    {
        if (state.predicted_damage >= state.local_health)
        {
            hp_value = "LETHAL";
            hp_color = IM_COL32(238, 68, 68, 255);
        }
        else
        {
            hp_value = std::to_string(std::max(0, state.local_health - state.predicted_damage));
            hp_color = IM_COL32(110, 225, 135, 255);
        }
    }
    const auto time_value = std::format("{:.1f} sec", state.time_remaining);
    draw_row(first_y, "Site:", site_value, bomb_u32);
    draw_row(first_y + line_height, "HP:", hp_value, hp_color);
    draw_row(first_y + line_height * 2.0f, "Time:", time_value, timer_color);
    draw->PopClipRect();
}

} // namespace ui::hud
