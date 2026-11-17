#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/constants.hpp>
#include <config/settings.hpp>
#include <render/presentation.hpp>
#include <game/state/services.hpp>
#include <config/misc.hpp>
#include <platform/windows/input.hpp>
#include <game/memory/bound_services.hpp>
#include <simulation/collision.hpp>
#include <game/input/bindings.hpp>
#include <diagnostics/journal.hpp>
#include <features/misc/misc.hpp>

namespace features::misc
{
namespace
{
[[nodiscard]] bool game_has_input_focus()
{
    const auto foreground = ::GetForegroundWindow();
    const auto root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD process_id{};
    if (root)
        ::GetWindowThreadProcessId(root, &process_id);
    return process_id != 0 && process_id == game::bound_memory().process.process_id();
}
} // namespace

void bhop_t::tick()
{
    const auto now = std::chrono::steady_clock::now();
    const auto release = [this, now]()
    {
        if (m_jump_down && platform::windows::bound_input().key(m_owned_jump_key, false))
        {
            m_jump_down = false;
            m_owned_jump_key = 0;
            m_last_transition = now;
        }
        return !m_jump_down;
    };
    const auto reset_bunny_sample = [this]
    {
        m_last_bunny_simulation_time = -1.0f;
        m_last_bunny_tap = {};
        m_bunny_pawn = 0;
    };
    const auto snapshot = config::get_runtime_snapshot();
    if (!snapshot)
    {
        platform::windows::bound_input().set_key_gate(0, false);
        m_gate_requested = false;
        release();
        m_edge_was_on_ground = false;
        m_edge_armed = false;
        reset_bunny_sample();
        return;
    }
    const auto& bunny_cfg = snapshot->general.m_bunny_hop;
    const auto& edge_cfg = snapshot->general.m_edge_jump;
    if (!bunny_cfg.enabled && !edge_cfg.enabled)
    {
        if (m_gate_requested)
        {
            const auto jump_key = m_jump_key;
            const auto game_active = game_has_input_focus();
            const auto player_ready = game::local_player().pawn() && game::local_player().alive();
            const auto restore = game_active && player_ready && !render::menu_open() &&
                                 !game::input_bindings().text_entry_active() &&
                                 platform::windows::bound_input().key_gate_held();
            if (!release())
                return;
            platform::windows::bound_input().set_key_gate(jump_key, false);
            if (restore && jump_key)
                platform::windows::bound_input().key(jump_key, true);
            m_gate_requested = false;
        }
        release();
        m_edge_was_on_ground = false;
        m_edge_armed = false;
        reset_bunny_sample();
        return;
    }

    const auto activation_key = static_cast<std::uint16_t>(std::clamp(bunny_cfg.activation_key, 0, 255));
    const auto jump_binding = game::input_bindings().resolve(game::input_action::jump);
    const auto resolved_key =
        jump_binding.device == game::input_device::keyboard ? jump_binding.virtual_key : std::uint16_t{};
    if (m_reported_jump_key != resolved_key)
    {
        m_reported_jump_key = resolved_key;
        diagnostics::emit(resolved_key ? diagnostics::event::bhop_binding_available
                                       : diagnostics::event::bhop_binding_unavailable);
    }
    if (m_jump_key != resolved_key)
    {
        if (!release())
            return;
        platform::windows::bound_input().set_key_gate(0, false);
        m_jump_key = resolved_key;
        m_gate_requested = false;
        m_last_bunny_simulation_time = -1.0f;
    }

    const auto game_active = game_has_input_focus();
    const auto pawn = game::local_player().pawn();
    const auto player_ready = pawn && game::local_player().alive();
    if (pawn != m_bunny_pawn)
    {
        if (!release())
            return;
        reset_bunny_sample();
        m_bunny_pawn = pawn;
        m_edge_was_on_ground = false;
        m_edge_armed = false;
    }
    const auto menu_open = render::menu_open();
    const auto text_entry_active = game::input_bindings().text_entry_active();
    const auto jump_key = m_jump_key;
    const auto activation_is_jump = activation_key != 0 && activation_key == jump_key;
    const auto activation_held =
        input_lifecycle::activation_held(activation_key, [](const std::uint16_t key)
                                         { return platform::windows::bound_input().physical_key_down(key); });
    const auto gate_requested = game_active && !menu_open && !text_entry_active && player_ready && bunny_cfg.enabled &&
                                jump_key != 0 && (activation_is_jump || activation_held);
    const auto restore_physical_jump = m_gate_requested && !gate_requested && game_active && !menu_open &&
                                       !text_entry_active && player_ready &&
                                       platform::windows::bound_input().key_gate_held();
    if (m_gate_requested && !gate_requested && !release())
        return;
    platform::windows::bound_input().set_key_gate(jump_key, gate_requested);
    if (restore_physical_jump && jump_key)
        platform::windows::bound_input().key(jump_key, true);
    m_gate_requested = gate_requested;

    const auto bunny_active = gate_requested && platform::windows::bound_input().key_gate_ready() &&
                              (activation_is_jump ? platform::windows::bound_input().key_gate_held() : activation_held);
    const auto edge_active =
        game_active && !menu_open && !text_entry_active && player_ready && edge_cfg.enabled && jump_key != 0 &&
        input_lifecycle::activation_held(edge_cfg.activation_key, [](const std::uint16_t key)
                                         { return platform::windows::bound_input().physical_key_down(key); });

    if (!player_ready || (!bunny_active && !edge_active))
    {
        release();
        m_edge_was_on_ground = false;
        m_edge_armed = false;
        reset_bunny_sample();
        return;
    }

    std::uint32_t flags{}, ground_entity{};
    if (!game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id), &flags, sizeof(flags)) ||
        !game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_hGroundEntity"_id), &ground_entity,
                                           sizeof(ground_entity)))
    {
        release();
        m_edge_was_on_ground = false;
        m_edge_armed = false;
        return;
    }
    const auto flags_on_ground = (flags & 1u) != 0u;
    const auto on_ground = flags_on_ground || ground_entity != 0xffffffffu;
    const auto since_transition = now - m_last_transition;

    constexpr auto k_airborne_min_hold = std::chrono::milliseconds(16);
    constexpr auto k_bunny_tap_interval = std::chrono::milliseconds(16);

    if (bunny_active)
    {
        if (!release())
            return;
        m_edge_was_on_ground = on_ground;
        m_edge_armed = false;

        if (!flags_on_ground)
            return;

        float simulation_time{};
        if (!game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_flSimulationTime"_id), &simulation_time,
                                               sizeof(simulation_time)) ||
            !std::isfinite(simulation_time) || simulation_time == m_last_bunny_simulation_time ||
            now - m_last_bunny_tap <= k_bunny_tap_interval)
        {
            return;
        }

        const std::array tap{
            platform::windows::input_gateway::key_transition{jump_key, true},
            platform::windows::input_gateway::key_transition{jump_key, false},
        };
        if (platform::windows::bound_input().keys(tap))
        {
            m_last_bunny_simulation_time = simulation_time;
            m_last_bunny_tap = now;
        }
        return;
    }

    if (m_jump_down && since_transition >= k_airborne_min_hold)
        release();

    if (on_ground && !m_edge_was_on_ground)
        m_edge_armed = true;

    auto trigger_edge_jump = false;
    if (m_edge_armed && !m_jump_down)
    {
        const auto sample_motion = [&](foundation::vec3& origin, foundation::vec3& velocity)
        {
            std::uintptr_t node{};
            return game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_pGameSceneNode"_id), &node,
                                                     sizeof(node)) &&
                   node &&
                   game::bound_memory().process.copy(node + SCHEMA("CGameSceneNode", "m_vecAbsOrigin"_id), &origin,
                                                     sizeof(origin)) &&
                   game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id), &velocity,
                                                     sizeof(velocity));
        };
        if (on_ground)
        {
            foundation::vec3 origin{}, velocity{};
            trigger_edge_jump = sample_motion(origin, velocity) && will_leave_ground(pawn, origin, velocity, true);
        }
        else if (m_edge_was_on_ground)
        {
            foundation::vec3 origin{}, velocity{};
            trigger_edge_jump = sample_motion(origin, velocity) && will_leave_ground(pawn, origin, velocity, false);
        }
    }

    if (trigger_edge_jump)
    {
        if (platform::windows::bound_input().key(jump_key, true))
        {
            m_owned_jump_key = jump_key;
            m_jump_down = true;
            m_last_transition = now;
            m_edge_armed = false;
        }
    }
    m_edge_was_on_ground = on_ground;
}

bool bhop_t::will_leave_ground(std::uintptr_t pawn, const foundation::vec3& origin, const foundation::vec3& velocity,
                               bool require_current_support) const
{
    if (!simulation::collision().valid() || velocity.length_2d() < 12.0f || velocity.z > 40.0f ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
    {
        return false;
    }

    const auto collision = pawn + SCHEMA("C_BaseModelEntity", "m_Collision"_id);
    foundation::vec3 mins{}, maxs{};
    if (!game::bound_memory().process.copy(collision + SCHEMA("CCollisionProperty", "m_vecMins"_id), &mins,
                                           sizeof(mins)) ||
        !game::bound_memory().process.copy(collision + SCHEMA("CCollisionProperty", "m_vecMaxs"_id), &maxs,
                                           sizeof(maxs)))
        return false;
    const auto extent_x = std::clamp(std::min(std::abs(mins.x), std::abs(maxs.x)) - 1.0f, 6.0f, 20.0f);
    const auto extent_y = std::clamp(std::min(std::abs(mins.y), std::abs(maxs.y)) - 1.0f, 6.0f, 20.0f);

    const auto has_walkable_support = [&](const foundation::vec3& base)
    {
        constexpr auto k_step_up = 20.0f;
        constexpr auto k_step_down = 24.0f;
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                const foundation::vec3 offset{extent_x * static_cast<float>(x) * 0.5f,
                                              extent_y * static_cast<float>(y) * 0.5f, 0.0f};
                const auto start = base + offset + foundation::vec3{0.0f, 0.0f, k_step_up};
                const auto end = base + offset - foundation::vec3{0.0f, 0.0f, k_step_down};
                const auto trace = simulation::collision().trace_ray(start, end);
                if (trace.hit && trace.normal.z >= 0.55f)
                    return true;
            }
        }
        return false;
    };

    if (require_current_support && !has_walkable_support(origin))
        return false;

    auto horizontal_velocity = velocity;
    horizontal_velocity.z = 0.0f;
    const auto predicted_origin = origin + horizontal_velocity * game::rules::simulation_step;
    if (has_walkable_support(predicted_origin))
        return false;

    const auto speed = horizontal_velocity.length_2d();
    const auto direction = horizontal_velocity / speed;
    constexpr std::array<float, 4> k_bridge_probes{4.0f, 8.0f, 12.0f, 16.0f};
    for (const auto distance : k_bridge_probes)
    {
        if (has_walkable_support(predicted_origin + direction * distance))
            return false;
    }
    return true;
}

} // namespace features::misc
