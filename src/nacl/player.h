#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "ppapi/c/ppb_opengles2.h"
#include "ppapi/cpp/audio.h"
#include "ppapi/cpp/graphics_3d.h"
#include "ppapi/cpp/graphics_3d_client.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/rect.h"
#include "ppapi/cpp/size.h"
#include "ppapi/cpp/video_decoder.h"
#include "ppapi/cpp/view.h"
#include "ppapi/utility/completion_callback_factory.h"

#include "core/session.h"

struct OpusDecoder;

namespace tsl {

// Samsung Tizen 3.0 NaCl media player: hardware-accelerated H.264 decoding via
// pp::VideoDecoder rendered to a full-screen OpenGL ES 2.0 quad (pp::Graphics3D),
// and Opus audio decoded via libopus to 16-bit 48 kHz stereo PCM played via pp::Audio.
class Player : public MediaSink {
public:
    enum class LatencyMode { Auto = 0, Low = 1, UltraLow = 2 };

    using VideoFailedCallback = std::function<void(bool restartNeeded)>;

    struct Stats {
        uint64_t videoAppended = 0;
        uint64_t videoFailed = 0;
        uint64_t audioAppended = 0;
        uint64_t audioFailed = 0;
        uint64_t audioConcealed = 0;
        uint64_t audioDropped = 0;
        uint64_t silenceFilled = 0;
        uint64_t audioGaps = 0;
        uint32_t audioCallbackFrames = 0;
        uint32_t maxFrameGapMs = 0;
        int64_t audioLeadMs = 0;
        uint64_t videoSessionChanges = 0;
        uint64_t videoTrackCloses = 0;
        int32_t lastCloseReason = -1;
        int32_t lastAppendError = 0;
        uint64_t pipelineErrors = 0;
        int32_t lastPipelineError = -1;
        uint64_t picturesDecoded = 0;
        uint64_t picturesPainted = 0;
        uint64_t picturesDropped = 0;
        int32_t latencyModeResult = -999;
    };

    Player(pp::Instance *instance, LatencyMode latency, VideoFailedCallback onVideoFailed);
    ~Player() override;

    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    void DidChangeView(const pp::View &view);

    bool SupportsHevc() override;
    static bool LegacyRuntime();
    static bool LatencyNeedsReload(bool hevc, LatencyMode mode);

    bool SetUp() const { return setUp_.load(); }
    bool restartNeeded() const { return false; }
    Stats stats() const;

    void RequestKeyframe();

    int VideoStart(const IHS_StreamVideoConfig &config) override;
    IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *data, size_t len, bool keyframe) override;
    void VideoStop() override;

    int AudioStart(const IHS_StreamAudioConfig &config) override;
    int AudioPacket(const uint8_t *data, size_t len) override;
    void AudioLost(uint32_t frames) override;
    void AudioStop() override;

private:
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioChannels = 2;
    // Preferred 5 ms quantum at 48 kHz; actual quantum is read from pp::AudioConfig::sample_frame_count().
    static constexpr uint32_t kAudioCallbackFrames = 240;
    // Fixed power-of-two SPSC stereo ring capacity (32768 frames ~= 682 ms at 48 kHz, ~128 KB).
    // Never reallocated across callbacks; steady-state latency is kept tight via consumer excess drain.
    static constexpr size_t kAudioRingCapacity = 32768;
    static constexpr size_t kAudioRingMask = kAudioRingCapacity - 1;
    // Cap queued compressed video access units to 4 frames (~66 ms @ 60 fps) before requesting IDR.
    static constexpr size_t kMaxQueuedVideoFrames = 4;

    struct Shader {
        uint32_t program = 0;
        int32_t texcoord_scale_location = 0;
        int32_t pos_location = -1;
        int32_t tc_location = -1;
    };

    // Video main-thread & worker callbacks
    void InitializeVideoOnMainThread(int32_t result, uint32_t width, uint32_t height);
    void OnDecoderInitialized(int32_t result, bool hwAccel);
    void ResetVideoDecoderOnMainThread(int32_t result);
    void StopVideoOnMainThread(int32_t result);
    void OnPictureReady(int32_t result, PP_VideoPicture picture);
    void PaintNextPicture();
    void OnSwapBuffersCompleted(int32_t result);
    void DecodeWorkerLoop();

    void CreateShaderProgram(int textureType);
    void CreateShader(uint32_t program, uint32_t type, const char *source, int size);
    void DeleteGL();
    std::vector<uint8_t> AcquireVideoBuffer(size_t reserveHint);
    void RecycleVideoBuffer(std::vector<uint8_t> &&buf);
    bool PrepareH264AccessUnit(const uint8_t *data, size_t len, bool keyframe, std::vector<uint8_t> &out);

    // Audio callback
    static void AudioPlayerSampleCallback(void *samples, uint32_t buffer_size, void *data);
    void FillAudioSamples(int16_t *out, uint32_t sample_count);
    void PushAudioFrames(const int16_t *pcm, size_t frames);
    void StartAudioOnMainThread(int32_t result);
    void StopAudioOnMainThread(int32_t result);

    pp::Instance *instance_;
    pp::CompletionCallbackFactory<Player> video_cb_factory_;
    pp::CompletionCallbackFactory<Player> audio_cb_factory_;
    VideoFailedCallback on_video_failed_;

    pp::Size viewport_size_{1920, 1080};
    pp::Size last_context_size_{0, 0};
    pp::Size last_viewport_size_{0, 0};
    pp::Size last_texture_size_{0, 0};
    pp::Graphics3D context_;
    pp::VideoDecoder *video_decoder_ = nullptr;
    Shader prog_2d_{};
    Shader prog_rect_{};
    Shader prog_ext_{};
    uint32_t vbo_ = 0;
    uint32_t last_texture_target_ = 0;
    uint32_t last_texture_id_ = 0;
    bool gl_initialized_ = false;
    bool painting_ = false;
    bool has_next_picture_ = false;
    PP_VideoPicture next_picture_{};
    PP_VideoPicture current_picture_{};
    uint32_t next_frame_number_ = 1;

    std::mutex video_mutex_;
    std::condition_variable video_cv_;
    std::deque<std::vector<uint8_t>> video_queue_;
    std::deque<std::vector<uint8_t>> video_pool_;
    std::thread decode_thread_;
    bool video_running_ = false;
    bool decoder_ready_ = false;
    bool decoder_failed_ = false;
    bool video_failed_reported_ = false;
    bool wait_for_keyframe_ = false;
    bool report_lost_needed_ = false;
    bool video_stop_pending_ = false;
    std::atomic<bool> keyframe_requested_{false};
    std::atomic<int64_t> last_video_frame_ms_{0};

    // Audio state
    std::mutex opus_mutex_;
    OpusDecoder *opus_ = nullptr;
    uint32_t audio_channels_ = 2;
    uint32_t last_opus_frames_ = 480;
    std::vector<int16_t> decode_pcm_;
    pp::Audio audio_player_;
    std::mutex audio_mutex_;
    std::condition_variable audio_cv_;
    std::vector<int16_t> audio_ring_;
    std::atomic<size_t> audio_read_frame_{0};
    std::atomic<size_t> audio_write_frame_{0};
    std::atomic<size_t> audio_trim_target_frames_{720};
    std::atomic<size_t> audio_max_target_frames_{1440};
    std::atomic<bool> audio_ring_active_{false};
    std::atomic<bool> audio_seen_packet_{false};
    bool audio_running_ = false;
    bool audio_player_ready_ = false;
    bool audio_stop_pending_ = false;

    std::atomic<bool> setUp_{false};
    std::atomic<uint64_t> videoAppended_{0};
    std::atomic<uint64_t> videoFailed_{0};
    std::atomic<uint64_t> audioAppended_{0};
    std::atomic<uint64_t> audioFailed_{0};
    std::atomic<uint64_t> audioConcealed_{0};
    std::atomic<uint64_t> audioDropped_{0};
    std::atomic<uint64_t> silenceFilled_{0};
    std::atomic<uint64_t> audioGaps_{0};
    std::atomic<uint32_t> audioCallbackFrames_{0};
    std::atomic<uint32_t> maxFrameGapMs_{0};
    std::atomic<int64_t> audioLeadMs_{0};
    std::atomic<uint64_t> videoSessionChanges_{0};
    std::atomic<uint64_t> videoTrackCloses_{0};
    std::atomic<int32_t> lastCloseReason_{-1};
    std::atomic<int32_t> lastAppendError_{0};
    std::atomic<uint64_t> pipelineErrors_{0};
    std::atomic<int32_t> lastPipelineError_{-1};
    std::atomic<uint64_t> picturesDecoded_{0};
    std::atomic<uint64_t> picturesPainted_{0};
    std::atomic<uint64_t> picturesDropped_{0};
    std::atomic<int32_t> latencyModeResult_{-999};
    std::atomic<bool> firstKeyLogged_{false};
};

}  // namespace tsl
