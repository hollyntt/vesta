#include <stdafx.hpp>

#include <features/misc/angle_placement.hpp>
#include <config/settings.hpp>
#include <game/state/services.hpp>
#include <render/presentation.hpp>
#include <simulation/collision.hpp>

namespace features::misc
{
namespace
{
[[nodiscard]] bool finite(const foundation::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool screen_valid(const foundation::vec2& point)
{
    return game::camera().projection_valid(point) &&
           std::isfinite(point.x) && std::isfinite(point.y) &&
           std::fabs(point.x) < 20000.0f && std::fabs(point.y) < 20000.0f;
}

[[nodiscard]] bool floor_below(const foundation::vec3& eye, foundation::vec3& ground)
{
    const auto floor = simulation::collision().trace_ray(
        eye, eye - foundation::vec3{0.0f, 0.0f, 100.0f});
    if (!floor.hit || !finite(floor.end_pos) || floor.normal.z < 0.55f)
        return false;
    ground = floor.end_pos + foundation::vec3{0.0f, 0.0f, 3.0f};
    return true;
}
} // namespace

void angle_placement_helper_t::resample(const foundation::vec3& eye,
                                        const foundation::vec3& forward,
                                        const float distance, const float spacing)
{
    wall_found_ = center_valid_ = left_valid_ = right_valid_ = false;
    last_eye_ = eye;
    if (!finite(eye) || !finite(forward) || !simulation::collision().valid())
        return;

    // A simple local geometry probe, independent of other players, combat,
    // line-of-fire predictions, and any movement/aim command.
    const auto wall = simulation::collision().trace_ray(eye, eye + forward * distance);
    if (!wall.hit || !finite(wall.end_pos) || !finite(wall.normal))
        return;

    const foundation::vec3 flat_normal{wall.normal.x, wall.normal.y, 0.0f};
    const auto len_sqr = flat_normal.length_sqr();
    if (len_sqr < 0.25f)
        return; // A floor/ceiling surface is not a useful lateral wall.

    wall_found_ = true;
    wall_point_ = wall.end_pos;
    wall_distance_ = (wall_point_ - eye).length();
    // The previous centre marker sat directly below the camera, which
    // generally projects behind the near plane. Move the visual footprint
    // forwards along the clear side of the wall.
    const auto lookahead = std::clamp(wall_distance_ * 0.55f, 8.0f, 120.0f);
    const auto marker_eye = eye + forward * lookahead;
    center_valid_ = floor_below(marker_eye, center_ground_);

    const foundation::vec3 tangent{
        -flat_normal.y / std::sqrt(len_sqr), flat_normal.x / std::sqrt(len_sqr), 0.0f};

    const auto try_lateral = [&](const foundation::vec3& direction, foundation::vec3& output) -> bool
    {
        const auto candidate_eye = marker_eye + direction * spacing;
        // A sphere clearance check avoids drawing directly through narrow
        // walls. This is a geometric preview, not a navigation guarantee.
        if (simulation::collision().sweep_sphere(marker_eye, candidate_eye, 11.0f).hit)
            return false;
        return floor_below(candidate_eye, output);
    };

    left_valid_ = try_lateral(tangent, left_ground_);
    right_valid_ = try_lateral(-tangent, right_ground_);
}

void angle_placement_helper_t::on_render(render::draw::draw_list& draw_list)
{
    const auto& cfg = config::general_settings.m_angle_placement;
    if (!cfg.enabled || !game::local_player().alive() || !game::local_player().pawn() ||
        !simulation::collision().valid())
    {
        wall_found_ = false;
        return;
    }

    const auto eye = game::camera().origin();
    const auto angles = game::camera().angles();
    if (!finite(eye) || !finite(angles))
        return;

    foundation::vec3 forward{}, right{}, up{};
    foundation::basis_from_angles(foundation::vec3{0.0f, angles.y, 0.0f}, forward, right, up);
    const auto now = std::chrono::steady_clock::now();
    const auto pawn = game::local_player().pawn();
    if (pawn != last_pawn_ || now >= next_refresh_ ||
        (eye - last_eye_).length_sqr() > 16.0f * 16.0f)
    {
        const auto range = std::clamp(cfg.scan_distance, 64.0f, 300.0f);
        const auto spacing = std::clamp(cfg.marker_spacing, 20.0f, 96.0f);
        resample(eye, forward, range, spacing);
        last_pawn_ = pawn;
        const auto update = std::clamp(cfg.refresh_seconds, 0.10f, 1.0f);
        next_refresh_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<float>(update));
    }
    if (!wall_found_)
        return;

    const auto anchor = game::camera().project(wall_point_);
    if (!screen_valid(anchor))
        return;

    if (cfg.show_wall_distance)
    {
        const auto eye_projected = game::camera().project(eye + forward * 24.0f);
        if (screen_valid(eye_projected))
            draw_list.add_line(eye_projected.x, eye_projected.y,
                               anchor.x, anchor.y, cfg.color, 1.5f);
    }
}
} // namespace features::misc
