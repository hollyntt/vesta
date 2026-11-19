#pragma once

#include <chrono>
#include <cstdint>

#include <foundation/math/vector.hpp>

namespace features::misc
{

// Auto Peek: while the player slides or peeks (moving, and firing), keep the crosshair anchored to the
// point they were pre-aimed at while standing still, so the aim does not drift off as they move. The view is
// steered by injecting relative mouse movement through the input gateway (this process-injection cheat cannot
// rewrite the user command). Ticked on the movement worker, the same thread that drives bhop and auto_stop.
class auto_peek_t
{
public:
    void tick();

private:
    // Resets the recorded anchor and view-driving state.
    void reset();

    std::uintptr_t m_pawn{};
    // Where the player was pre-aimed from; zero (unset) until a still spot is recorded.
    foundation::vec3 m_anchor{};
    bool m_anchor_valid{false};
    // Remembers the last injected mouse delta so a steady error is not re-sent every tick.
    foundation::vec2 m_mouse_remainder{};
};

inline auto_peek_t& auto_peek()
{
    static auto_peek_t value{};
    return value;
}

} // namespace features::misc
