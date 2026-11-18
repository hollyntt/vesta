#pragma once

#include <chrono>
#include <cstdint>

#include <foundation/math/vector.hpp>
#include <render/draw.hpp>

namespace features::misc
{
// Render-thread-only, visual guide for local wall geometry.
// Never accesses enemy snapshots or sends input commands.
class angle_placement_helper_t final
{
public:
    void on_render(render::draw::draw_list& draw_list);

private:
    void resample(const foundation::vec3& eye, const foundation::vec3& forward,
                  float distance, float spacing);

    std::chrono::steady_clock::time_point next_refresh_{};
    std::uintptr_t last_pawn_{};
    foundation::vec3 last_eye_{};
    foundation::vec3 wall_point_{};
    foundation::vec3 center_ground_{};
    foundation::vec3 left_ground_{};
    foundation::vec3 right_ground_{};
    bool wall_found_{};
    bool center_valid_{};
    bool left_valid_{};
    bool right_valid_{};
    float wall_distance_{};
};

inline angle_placement_helper_t& angle_placement_helper()
{
    static angle_placement_helper_t value{};
    return value;
}
} // namespace features::misc
