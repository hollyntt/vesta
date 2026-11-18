#pragma once

#include <memory>

namespace features::visuals
{

class hitsound_player
{
public:
    hitsound_player();
    ~hitsound_player();
    hitsound_player(const hitsound_player&) = delete;
    hitsound_player& operator=(const hitsound_player&) = delete;

    void play(int style, float volume);
    void play(int style, float volume, const char* custom_path);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

inline hitsound_player& hitsounds()
{
    static hitsound_player value{};
    return value;
}

// Same playback engine as hitsound_player, but custom style 5 reads m_custom_killsound
// instead of m_custom_hitsound.
class killsound_player
{
public:
    killsound_player();
    ~killsound_player();
    killsound_player(const killsound_player&) = delete;
    killsound_player& operator=(const killsound_player&) = delete;

    void play(int style, float volume);
    void play(int style, float volume, const char* custom_path);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

inline killsound_player& killsounds()
{
    static killsound_player value{};
    return value;
}

} // namespace features::visuals
