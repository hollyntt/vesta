#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/constants.hpp>
#include <config/settings.hpp>
#include <render/presentation.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <game/input/action_state.hpp>
#include <platform/windows/input.hpp>
#include <features/misc/auto_peek.hpp>
#include <features/aimbot/aimbot.hpp>

#include <algorithm>

namespace features::misc
{

void auto_peek_t::reset()
{
    m_anchor = {};
    m_anchor_valid = false;
    m_mouse_remainder = {};
}

void auto_peek_t::tick()
{
    const auto snapshot = config::get_runtime_snapshot();
    if (!snapshot || !snapshot->general.m_auto_peek.enabled)
    {
        reset();
        return;
    }
    const auto& cfg = snapshot->general.m_auto_peek;

    // Autofire or triggerbot activation also counts as an activate reason;
    // otherwise respect the per-config key + mode gate.
    const bool external_fire = features::aimbot::g_walk_autofire.load(std::memory_order_acquire) ||
                               (snapshot->general.m_autofire.enabled &&
                                config::combat_profile::activation_active(
                                    snapshot->general.m_autofire.activation_mode,
                                    snapshot->general.m_autofire.key));
    const bool self_activated =
        config::combat_profile::activation_active(cfg.activation_mode, cfg.activation_key);
    const bool activated = external_fire || self_activated;

    const auto pawn = game::local_player().pawn();
    if (!pawn || !game::local_player().alive() ||
        !game::rules::is_firearm(game::local_player().weapon_type()) ||
        render::menu_open() || game::input_bindings().text_entry_active())
    {
        reset();
        m_pawn = pawn;
        return;
    }
    if (pawn != m_pawn)
    {
        reset();
        m_pawn = pawn;
    }

    std::uint32_t flags{};
    foundation::vec3 origin{}, velocity{};
    if (!game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id), &flags, sizeof(flags)) ||
        !game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsOrigin"_id), &origin, sizeof(origin)) ||
        !game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id), &velocity,
                                          sizeof(velocity)))
    {
        reset();
        return;
    }
    const bool on_ground = (flags & 1u) != 0u;
    const auto speed_2d = velocity.length_2d();
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
    {
        reset();
        return;
    }

    const auto eye = game::camera().origin();
    auto view_angles = game::camera().angles();
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
    {
        reset();
        return;
    }
    foundation::vec3 forward{};
    view_angles.to_directions(&forward, nullptr, nullptr);
    forward.z = 0.0f;
    if (const auto forward_length = forward.length_2d(); forward_length > 1e-3f)
        forward /= forward_length;

    // Still enough to (re)record the pre-aimed anchor: a point peek_distance in front of the eye, at the
    // player's eye height. Re-arms whenever they slow down, so the next peek starts fresh.
    const auto record_speed = std::clamp(cfg.record_speed, 0.0f, 100.0f);
    if (on_ground && speed_2d <= record_speed)
    {
        const auto peek_distance = std::clamp(cfg.peek_distance, 0.0f, 2000.0f);
        m_anchor = origin + forward * peek_distance + foundation::vec3{0.0f, 0.0f, 2.0f};
        m_anchor_valid = true;
        m_mouse_remainder = {};
        return;
    }

    // Moving. Without a recorded anchor there is nothing to hold the crosshair on.
    if (!m_anchor_valid)
        return;

    const bool firing = platform::windows::bound_input().injected_primary_down() ||
                        game::physical_action_down(game::input_action::attack,
                                                   platform::windows::bound_input());
    // When externally activated (autofire/triggerbot), bypass the physical-firing gate so the peek still engages
    // even if no physical mouse button is held. Otherwise respect require_firing.
    if (!activated && cfg.require_firing && !firing)
        return;

    const auto to_anchor = m_anchor - origin;
    foundation::vec3 flat_to_anchor = to_anchor;
    flat_to_anchor.z = 0.0f;
    // Only steer toward an anchor that is roughly in front; a flipped view would fling the crosshair 180.
    if (flat_to_anchor.dot(forward) <= 0.0f)
        return;

    const auto target_yaw = foundation::to_degrees(std::atan2(to_anchor.y, to_anchor.x));
    const auto target_pitch =
        std::clamp(foundation::to_degrees(std::atan2(to_anchor.z, std::hypot(to_anchor.x, to_anchor.y))), -80.0f, 80.0f);
    const auto error_yaw = foundation::wrap_yaw(target_yaw - view_angles.y);
    const auto error_pitch = target_pitch - view_angles.x;
    if (std::hypot(error_yaw, error_pitch) < 0.15f)
        return;

    const auto sensitivity = game::variables().get<float>(CONVAR("sensitivity"_id));
    const auto fov_adjust =
        game::bound_memory().process.load<float>(pawn + SCHEMA("C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_id));
    const auto per_count = sensitivity * 0.022f * (fov_adjust > 0.0f && std::isfinite(fov_adjust) ? fov_adjust : 1.0f);
    if (!(per_count > 0.0f) || !std::isfinite(per_count))
        return;

    const auto hold = std::clamp(cfg.hold, 1.0f, 100.0f) * 0.01f;
    const auto yaw_step = error_yaw * hold;
    const auto pitch_step = error_pitch * hold;
    m_mouse_remainder.x += -yaw_step / per_count;
    m_mouse_remainder.y += pitch_step / per_count;
    const auto dx = std::clamp(static_cast<int>(m_mouse_remainder.x), -32, 32);
    const auto dy = std::clamp(static_cast<int>(m_mouse_remainder.y), -32, 32);
    m_mouse_remainder.x -= static_cast<float>(dx);
    m_mouse_remainder.y -= static_cast<float>(dy);
    if (dx || dy)
        platform::windows::bound_input().pointer(dx, dy, platform::windows::pointer_action::relative_move);
}

} // namespace features::misc
