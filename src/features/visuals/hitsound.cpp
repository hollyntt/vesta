#include <stdafx.hpp>
#include <features/visuals/hitsound.hpp>
#include <resources/sounds/hitsounds.hpp>
#include <resources/sounds/killsounds.hpp>
#include <config/settings.hpp>
#include <diagnostics/journal.hpp>
#include <filesystem>
#include <fstream>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <xaudio2.h>
#include <objbase.h>
#include <condition_variable>
#include <deque>

namespace features::visuals
{

namespace
{

constexpr std::uint16_t wave_format_extensible_tag = 0xFFFE;
constexpr std::uint16_t wave_format_adpcm_tag = 0x0002; // MS ADPCM, decoded to PCM16 in parse_wav

// Passed as the `code` argument of diagnostics::emit so each failure is distinguishable
// in the journal. CreateSourceVoice failures pass the raw HRESULT instead.
namespace failure
{
constexpr std::uint32_t engine_init = 0x48530001;      // 'HS' 0001
constexpr std::uint32_t custom_disabled = 0x48530002;  // disabled or empty path, or no config snapshot
constexpr std::uint32_t file_not_found = 0x48530003;
constexpr std::uint32_t file_open = 0x48530004;
constexpr std::uint32_t parse_failed = 0x48530005;     // unsupported or corrupt WAV
constexpr std::uint32_t embedded_parse = 0x48530006;
constexpr std::uint32_t submit_failed = 0x48530007;
} // namespace failure

struct wav_view
{
    // A WAVEFORMATEX followed by the extra bytes ADPCM needs (cbSize bytes after the 18-byte header).
    std::array<BYTE, 64> format_bytes{};
    const BYTE* samples{};
    UINT32 sample_bytes{};
    // Owns the decoded PCM when the source was ADPCM (samples then points into it).
    std::shared_ptr<std::vector<unsigned char>> pcm{};

    [[nodiscard]] WAVEFORMATEX* wave_format() { return reinterpret_cast<WAVEFORMATEX*>(format_bytes.data()); }
    [[nodiscard]] const WAVEFORMATEX* wave_format() const
    {
        return reinterpret_cast<const WAVEFORMATEX*>(format_bytes.data());
    }
};

[[nodiscard]] std::uint16_t read_u16(const unsigned char* value)
{
    return static_cast<std::uint16_t>(value[0] | value[1] << 8);
}

[[nodiscard]] std::uint32_t read_u32(const unsigned char* value)
{
    return static_cast<std::uint32_t>(value[0]) | static_cast<std::uint32_t>(value[1]) << 8 |
           static_cast<std::uint32_t>(value[2]) << 16 | static_cast<std::uint32_t>(value[3]) << 24;
}

// Decodes Microsoft ADPCM (4-bit, mono or stereo) to interleaved 16-bit PCM. XAudio2 rejected the raw format,
// so it is expanded here. The seven standard coefficient pairs are used, as every MS ADPCM file does.
[[nodiscard]] std::shared_ptr<std::vector<unsigned char>> decode_ms_adpcm(const wav_view& wav)
{
    static constexpr int coef1[7] = {256, 512, 0, 192, 240, 460, 392};
    static constexpr int coef2[7] = {0, -256, 0, 64, 0, -208, -232};
    static constexpr int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

    const auto* format = wav.wave_format();
    const std::size_t channels = format->nChannels;
    const std::size_t block = format->nBlockAlign;
    if (channels < 1 || channels > 2 || block < 7 * channels)
        return nullptr;

    auto pcm = std::make_shared<std::vector<unsigned char>>();
    pcm->reserve(static_cast<std::size_t>(wav.sample_bytes) * 4);

    const auto put = [&](int sample)
    {
        pcm->push_back(static_cast<unsigned char>(sample & 0xFF));
        pcm->push_back(static_cast<unsigned char>((sample >> 8) & 0xFF));
    };

    for (std::size_t offset = 0; offset + 7 * channels <= wav.sample_bytes; offset += block)
    {
        const unsigned char* src = wav.samples + offset;
        const std::size_t length = std::min<std::size_t>(block, wav.sample_bytes - offset);

        int predictor[2]{};
        int delta[2]{};
        int s1[2]{};
        int s2[2]{};
        std::size_t pos = 0;
        for (std::size_t c = 0; c < channels; ++c)
            predictor[c] = std::min<int>(src[pos++], 6);
        for (std::size_t c = 0; c < channels; ++c, pos += 2)
            delta[c] = static_cast<std::int16_t>(read_u16(src + pos));
        for (std::size_t c = 0; c < channels; ++c, pos += 2)
            s1[c] = static_cast<std::int16_t>(read_u16(src + pos));
        for (std::size_t c = 0; c < channels; ++c, pos += 2)
            s2[c] = static_cast<std::int16_t>(read_u16(src + pos));

        for (std::size_t c = 0; c < channels; ++c)
            put(s2[c]);
        for (std::size_t c = 0; c < channels; ++c)
            put(s1[c]);

        std::size_t channel = 0;
        for (; pos < length; ++pos)
        {
            for (int half = 0; half < 2; ++half)
            {
                const int nibble = half == 0 ? src[pos] >> 4 : src[pos] & 0x0F;
                const int guess =
                    (s1[channel] * coef1[predictor[channel]] + s2[channel] * coef2[predictor[channel]]) >> 8;
                const int sample = std::clamp(guess + (nibble >= 8 ? nibble - 16 : nibble) * delta[channel],
                                              -32768, 32767);
                s2[channel] = s1[channel];
                s1[channel] = sample;
                delta[channel] = std::max(16, (adapt[nibble] * delta[channel]) >> 8);
                put(sample);
                channel = (channel + 1) % channels;
            }
        }
    }

    if (pcm->empty())
        return nullptr;
    return pcm;
}

[[nodiscard]] std::optional<wav_view> parse_wav(const unsigned char* bytes, std::size_t size)
{
    if (!bytes || size < 44 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
    {
        return std::nullopt;
    }

    wav_view result{};
    bool have_format{};
    for (std::size_t cursor = 12; cursor + 8 <= size;)
    {
        auto chunk_size = static_cast<std::size_t>(read_u32(bytes + cursor + 4));
        const auto data = cursor + 8;
        if (data > size)
            break;

        const bool is_data_chunk = std::memcmp(bytes + cursor, "data", 4) == 0;
        const auto available = size - data;
        if (chunk_size > available)
        {
            // Some writers (streaming encoders, broken tools) emit a data size of 0,
            // 0xFFFFFFFF or one that overshoots the file. Clamp for the data chunk only.
            if (is_data_chunk)
                chunk_size = available;
            else
                break;
        }

        if (std::memcmp(bytes + cursor, "fmt ", 4) == 0 && chunk_size >= 16)
        {
            result.wave_format()->wFormatTag = read_u16(bytes + data);
            result.wave_format()->nChannels = read_u16(bytes + data + 2);
            result.wave_format()->nSamplesPerSec = read_u32(bytes + data + 4);
            result.wave_format()->nAvgBytesPerSec = read_u32(bytes + data + 8);
            result.wave_format()->nBlockAlign = read_u16(bytes + data + 12);
            result.wave_format()->wBitsPerSample = read_u16(bytes + data + 14);
            result.wave_format()->cbSize = 0;

            if (result.wave_format()->wFormatTag == wave_format_extensible_tag)
            {
                // WAVE_FORMAT_EXTENSIBLE: real format is the first 2 bytes of the SubFormat GUID
                // (fmt chunk offset 24). Flatten it to a plain PCM / float format.
                if (chunk_size >= 40)
                    result.wave_format()->wFormatTag = read_u16(bytes + data + 24);
                else
                    result.wave_format()->wFormatTag = 0;
            }

            have_format = result.wave_format()->wFormatTag == WAVE_FORMAT_PCM ||
                          result.wave_format()->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                          result.wave_format()->wFormatTag == wave_format_adpcm_tag;
        }
        else if (is_data_chunk)
        {
            result.samples = reinterpret_cast<const BYTE*>(bytes + data);
            result.sample_bytes = static_cast<UINT32>(chunk_size);
        }
        cursor = data + chunk_size + (chunk_size & 1u);
    }

    if (!have_format || !result.samples || !result.sample_bytes || !result.wave_format()->nChannels ||
        !result.wave_format()->nSamplesPerSec)
    {
        return std::nullopt;
    }

    if (result.wave_format()->wFormatTag == wave_format_adpcm_tag)
    {
        auto pcm = decode_ms_adpcm(result);
        if (!pcm)
            return std::nullopt;

        auto* format = result.wave_format();
        format->wFormatTag = WAVE_FORMAT_PCM;
        format->wBitsPerSample = 16;
        format->nBlockAlign = static_cast<WORD>(format->nChannels * 2);
        format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
        format->cbSize = 0;
        result.pcm = std::move(pcm);
        result.samples = result.pcm->data();
        result.sample_bytes = static_cast<UINT32>(result.pcm->size());
    }

    // Trim any trailing partial frame.
    if (result.wave_format()->nBlockAlign)
        result.sample_bytes -= result.sample_bytes % result.wave_format()->nBlockAlign;
    if (!result.sample_bytes)
        return std::nullopt;

    return result;
}

// The config path may be UTF-8 (e.g. from a wide file dialog) or the ANSI code page.
// std::filesystem::path(const char*) always assumes ANSI, which breaks non-ASCII paths.
[[nodiscard]] std::filesystem::path path_from_config(const char* text)
{
    if (!text || !*text)
        return {};

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length > 0)
    {
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), length);
        wide.resize(static_cast<std::size_t>(length) - 1);
        return std::filesystem::path(wide);
    }

    // Not valid UTF-8: fall back to the ANSI code page.
    const int ansi_length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (ansi_length > 0)
    {
        std::wstring wide(static_cast<std::size_t>(ansi_length), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), ansi_length);
        wide.resize(static_cast<std::size_t>(ansi_length) - 1);
        return std::filesystem::path(wide);
    }
    return std::filesystem::path(text);
}

void debug_log(const std::string& message)
{
    OutputDebugStringA(("[hitsound] " + message + "\n").c_str());
}

struct embedded_sound
{
    const unsigned char* bytes{};
    std::size_t size{};
};

constexpr std::array hit_sounds{embedded_sound{resources::sounds::soft, resources::sounds::soft_size},
                                embedded_sound{resources::sounds::glass, resources::sounds::glass_size},
                                embedded_sound{resources::sounds::pluck, resources::sounds::pluck_size},
                                embedded_sound{resources::sounds::crisp, resources::sounds::crisp_size},
                                embedded_sound{resources::sounds::flesh, resources::sounds::flesh_size}};

constexpr std::array kill_sounds{embedded_sound{resources::sounds::bell, resources::sounds::bell_size},
                                 embedded_sound{resources::sounds::shut, resources::sounds::shut_size},
                                 embedded_sound{resources::sounds::rise, resources::sounds::rise_size},
                                 embedded_sound{resources::sounds::buzz, resources::sounds::buzz_size},
                                 embedded_sound{resources::sounds::snap, resources::sounds::snap_size}};

} // namespace

template <typename Config>
struct sound_engine
{
    using custom_config_fn = const Config& (*)(const config::runtime_snapshot&);

    struct request
    {
        int style{};
        float volume{};
        const char* custom_path{};
    };
    struct voice
    {
        IXAudio2SourceVoice* source{};
        // Keeps custom WAV bytes alive for as long as XAudio2 may read them.
        std::shared_ptr<std::vector<unsigned char>> storage{};
    };

    std::mutex mutex{};
    std::condition_variable wake{};
    std::deque<request> requests{};
    bool stopping{};
    IXAudio2* engine{};
    IXAudio2MasteringVoice* mastering{};
    std::vector<voice> voices{};
    std::thread worker{};
    custom_config_fn custom_config{};
    const char* log_tag{"[hitsound] "};
    const std::array<embedded_sound, 5>* clips{&hit_sounds};

    explicit sound_engine(custom_config_fn config, const char* tag, const std::array<embedded_sound, 5>* embedded)
        : custom_config(config)
        , log_tag(tag)
        , clips(embedded)
    {
        worker = std::thread([this] { run(); });
    }

    ~sound_engine()
    {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
        // Voices must be destroyed before their storage is released (storage is released
        // when `voices` is destroyed, after this loop).
        for (auto& voice : voices)
        {
            if (voice.source)
                voice.source->DestroyVoice();
        }
        voices.clear();
        if (mastering)
            mastering->DestroyVoice();
        if (engine)
            engine->Release();
    }

    void enqueue(int style, float volume, const char* custom_path = nullptr)
    {
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                return;
            if (requests.size() >= 32)
                requests.pop_front();
            requests.push_back({style, volume, custom_path});
        }
        wake.notify_one();
    }

    void run()
    {
        // XAudio2 requires COM on this thread.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            request next{};
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !requests.empty(); });
                if (stopping && requests.empty())
                    break;
                next = requests.front();
                requests.pop_front();
            }
                play_now(next.style, next.volume, next.custom_path);
        }
        CoUninitialize();
    }

    bool initialize()
    {
        if (engine)
            return mastering != nullptr;
        if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
            return false;
        if (FAILED(engine->CreateMasteringVoice(&mastering)))
        {
            engine->Release();
            engine = nullptr;
            return false;
        }
        voices.reserve(16);
        return true;
    }

    void collect_finished()
    {
        std::erase_if(voices,
                      [](voice& value)
                      {
                          XAUDIO2_VOICE_STATE state{};
                          value.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                          if (state.BuffersQueued)
                              return false;
                          value.source->DestroyVoice();
                          return true;
                      });
    }

    void log(const std::string& message) const
    {
        OutputDebugStringA((std::string(log_tag) + message + "\n").c_str());
    }

    void play_now(int style, float volume, const char* custom_path = nullptr)
    {
        if (!initialize())
        {
            diagnostics::emit(diagnostics::event::bhop_binding_unavailable, diagnostics::severity::warning,
                              diagnostics::code_domain::mode, failure::engine_init);
            return;
        }

        collect_finished();

        std::optional<wav_view> wave;
        // Declared in the outer scope so the bytes outlive parse_wav's returned pointers.
        std::shared_ptr<std::vector<unsigned char>> storage;
        std::uint32_t failure_code = failure::custom_disabled;

        if (style >= 0 && style < static_cast<int>(clips->size()))
        {
            // Embedded sounds (0-4). Hit and kill players use different banks.
            const auto& clip = (*clips)[static_cast<std::size_t>(style)];
            wave = parse_wav(clip.bytes, clip.size);
            failure_code = failure::embedded_parse;
        }
        else
        {
            // Custom sound (style == 5)
            const char* custom_cfg_path = custom_path;
            if (!custom_cfg_path)
            {
                const auto snapshot = config::get_runtime_snapshot();
                if (snapshot)
                {
                    const auto& custom_cfg = custom_config(*snapshot);
                    custom_cfg_path = custom_cfg.enabled && custom_cfg.path.data()[0] != '\0'
                                          ? custom_cfg.path.data()
                                          : nullptr;
                }
            }
            if (custom_cfg_path && custom_cfg_path[0] != '\0')
            {
                const auto path = path_from_config(custom_cfg_path);
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                {
                    failure_code = failure::file_not_found;
                    log("custom file not found: " + std::string(custom_cfg_path));
                }
                else
                {
                    std::ifstream file(path, std::ios::binary);
                    if (!file)
                    {
                        failure_code = failure::file_open;
                        log("custom file open failed: " + std::string(custom_cfg_path));
                    }
                    else
                    {
                        storage = std::make_shared<std::vector<unsigned char>>(
                            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                        wave = parse_wav(storage->data(), storage->size());
                        if (!wave)
                        {
                            failure_code = failure::parse_failed;
                            log("custom file is not a supported WAV (" +
                                std::to_string(storage->size()) + " bytes)");
                            if (storage->size() >= 36 && std::memcmp(storage->data(), "RIFF", 4) == 0)
                            {
                                // Best-effort header dump to help diagnose unsupported formats.
                                log("  wFormatTag=" + std::to_string(read_u16(storage->data() + 20)) +
                                          " channels=" + std::to_string(read_u16(storage->data() + 22)) +
                                          " rate=" + std::to_string(read_u32(storage->data() + 24)) +
                                          " bits=" + std::to_string(read_u16(storage->data() + 34)));
                            }
                        }
                    }
                }
            }
            else
            {
                log("custom sound disabled or path empty");
            }
        }

        // Decoded ADPCM lives in the wav_view; hand it to the voice so it outlives this call.
        if (wave && wave->pcm)
            storage = wave->pcm;

        if (!wave)
        {
            diagnostics::emit(diagnostics::event::bhop_binding_unavailable, diagnostics::severity::warning,
                              diagnostics::code_domain::mode, failure_code);
            return;
        }

        log("loaded: tag=" + std::to_string(wave->wave_format()->wFormatTag) +
                  " bits=" + std::to_string(wave->wave_format()->wBitsPerSample) +
                  " channels=" + std::to_string(wave->wave_format()->nChannels) +
                  " rate=" + std::to_string(wave->wave_format()->nSamplesPerSec) +
                  " pcm_bytes=" + std::to_string(wave->sample_bytes) + " (build: adpcm-decode)");

        diagnostics::emit(diagnostics::event::bhop_binding_available, diagnostics::severity::information,
                          diagnostics::code_domain::mode);

        if (voices.size() >= 16)
        {
            auto& oldest = voices.front();
            oldest.source->Stop(0);
            oldest.source->DestroyVoice();
            voices.erase(voices.begin());
        }

        IXAudio2SourceVoice* source{};
        const HRESULT create_result = engine->CreateSourceVoice(&source, wave->wave_format());
        if (FAILED(create_result))
        {
            log("CreateSourceVoice failed, hr=" + std::to_string(static_cast<long>(create_result)) +
                      " tag=" + std::to_string(wave->wave_format()->wFormatTag) +
                      " bits=" + std::to_string(wave->wave_format()->wBitsPerSample) +
                      " rate=" + std::to_string(wave->wave_format()->nSamplesPerSec) +
                      " channels=" + std::to_string(wave->wave_format()->nChannels));
            diagnostics::emit(diagnostics::event::bhop_binding_unavailable, diagnostics::severity::error,
                              diagnostics::code_domain::mode, static_cast<std::uint32_t>(create_result));
            return;
        }
        source->SetVolume(std::clamp(volume, 0.0f, 1.0f));
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = wave->sample_bytes;
        buffer.pAudioData = wave->samples;
        if (FAILED(source->SubmitSourceBuffer(&buffer)) || FAILED(source->Start(0)))
        {
            source->DestroyVoice();
            diagnostics::emit(diagnostics::event::bhop_binding_unavailable, diagnostics::severity::error,
                              diagnostics::code_domain::mode, failure::submit_failed);
            return;
        }
        voices.push_back({source, std::move(storage)});
    }
};

struct hitsound_player::implementation
    : sound_engine<config::general_profile::custom_hitsound>
{
    implementation()
        : sound_engine([](const config::runtime_snapshot& snapshot) -> const config::general_profile::custom_hitsound& {
              return snapshot.general.m_custom_hitsound;
          },
          "[hitsound] ", &hit_sounds)
    {
    }
};

struct killsound_player::implementation
    : sound_engine<config::general_profile::custom_killsound>
{
    implementation()
        : sound_engine([](const config::runtime_snapshot& snapshot) -> const config::general_profile::custom_killsound& {
              return snapshot.general.m_custom_killsound;
          },
          "[killsound] ", &kill_sounds)
    {
    }
};

hitsound_player::hitsound_player()
    : m_impl(std::make_unique<implementation>())
{
}
hitsound_player::~hitsound_player() = default;

void hitsound_player::play(int style, float volume)
{
    play(style, volume, nullptr);
}

void hitsound_player::play(int style, float volume, const char* custom_path)
{
    debug_log("play() style=" + std::to_string(style) + " volume=" + std::to_string(volume));
    if (!m_impl || volume <= 0.0f)
        return;
    m_impl->enqueue(style, std::clamp(volume, 0.0f, 1.0f), custom_path);
}

killsound_player::killsound_player()
    : m_impl(std::make_unique<implementation>())
{
}
killsound_player::~killsound_player() = default;

void killsound_player::play(int style, float volume)
{
    play(style, volume, nullptr);
}

void killsound_player::play(int style, float volume, const char* custom_path)
{
    OutputDebugStringA(("[killsound] play() style=" + std::to_string(style) + " volume=" + std::to_string(volume) + "\n").c_str());
    if (!m_impl || volume <= 0.0f)
        return;
    m_impl->enqueue(style, std::clamp(volume, 0.0f, 1.0f), custom_path);
}

} // namespace features::visuals