#include "nacl/player.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/samsung/video_decoder_samsung.h"
#include "ppapi/lib/gl/gles2/gl2ext_ppapi.h"

#include "core/log.h"
#include "nacl/h264bitstream/h264_stream.h"

namespace tsl {
namespace {

int64_t SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Rewrites an H.264 SPS NALU to set num_ref_frames = 1 and max_dec_frame_buffering = 1
// so Samsung Tizen's hardware decoder outputs frames immediately without buffering.
void WriteModifiedSps(h264_stream_t *stream, const uint8_t *nal_data, int nal_len,
                      std::vector<uint8_t> &out) {
    read_nal_unit(stream, const_cast<uint8_t *>(nal_data), nal_len);

    int orig_level = stream->sps->level_idc;
    int orig_refs = stream->sps->num_ref_frames;
    int orig_br = stream->sps->vui.bitstream_restriction_flag;
    int orig_reorder = stream->sps->vui.num_reorder_frames;
    int orig_dpb = stream->sps->vui.max_dec_frame_buffering;

    if (stream->sps->level_idc > 42) {
        stream->sps->level_idc = 42;
    }
    stream->sps->num_ref_frames = 1;
    stream->sps->vui_parameters_present_flag = 1;
    if (!stream->sps->vui.bitstream_restriction_flag) {
        stream->sps->vui.bitstream_restriction_flag = 1;
        stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
        stream->sps->vui.max_bytes_per_pic_denom = 0;
        stream->sps->vui.max_bits_per_mb_denom = 0;
        stream->sps->vui.log2_max_mv_length_horizontal = 16;
        stream->sps->vui.log2_max_mv_length_vertical = 16;
    }
    // Always force 0 reorder frames and 1 frame max DPB buffering even when Steam's
    // SPS already had bitstream_restriction_flag == 1.
    stream->sps->vui.num_reorder_frames = 0;
    stream->sps->vui.max_dec_frame_buffering = 1;

    const size_t max_out = static_cast<size_t>(nal_len) + 64;
    std::vector<uint8_t> tmp(max_out, 0);
    int written = write_nal_unit(stream, tmp.data(), static_cast<int>(max_out));
    Log(IHS_LogLevelInfo, "Player",
        "SPS patch: level %d->%d, refs %d->1, br %d->1, reorder %d->0, dpb %d->1, len %d->%d",
        orig_level, stream->sps->level_idc, orig_refs, orig_br, orig_reorder, orig_dpb,
        nal_len, written);
    if (written <= 1) {
        const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
        out.insert(out.end(), start_code, start_code + 4);
        out.insert(out.end(), nal_data, nal_data + nal_len);
        return;
    }
    const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
    out.insert(out.end(), start_code, start_code + 4);
    out.insert(out.end(), tmp.data(), tmp.data() + written);
}

}  // namespace

Player::Player(pp::Instance *instance, LatencyMode latency, VideoFailedCallback onVideoFailed)
    : instance_(instance),
      video_cb_factory_(this),
      audio_cb_factory_(this),
      on_video_failed_(std::move(onVideoFailed)),
      audio_ring_(kAudioRingCapacity * kAudioChannels, 0) {
    (void) latency;
}

Player::~Player() {
    VideoStop();
    AudioStop();
    if (pp::Module::Get()->core()->IsMainThread()) {
        DeleteGL();
        context_ = pp::Graphics3D();
    }
}

std::vector<uint8_t> Player::AcquireVideoBuffer(size_t reserveHint) {
    if (!video_pool_.empty()) {
        std::vector<uint8_t> buf = std::move(video_pool_.front());
        video_pool_.pop_front();
        buf.clear();
        if (buf.capacity() < reserveHint) buf.reserve(reserveHint);
        return buf;
    }
    std::vector<uint8_t> buf;
    buf.reserve(reserveHint);
    return buf;
}

void Player::RecycleVideoBuffer(std::vector<uint8_t> &&buf) {
    buf.clear();
    if (video_pool_.size() < 4 && buf.capacity() <= 2 * 1024 * 1024) {
        video_pool_.push_back(std::move(buf));
    }
}

bool Player::PrepareH264AccessUnit(const uint8_t *data, size_t len, bool keyframe,
                                   std::vector<uint8_t> &out) {
    out.clear();
    if (!keyframe || len < 5) {
        out.assign(data, data + len);
        return true;
    }

    h264_stream_t *stream = h264_new();
    if (!stream) {
        out.assign(data, data + len);
        return true;
    }
    if (out.capacity() < len + 64) {
        out.reserve(len + 64);
    }

    size_t offset = 0;
    while (offset < len) {
        int nal_start = 0;
        int nal_end = 0;
        int rem = static_cast<int>(len - offset);
        int rc = find_nal_unit(const_cast<uint8_t *>(data + offset), rem, &nal_start, &nal_end);
        if (rc == 0 || nal_start <= 0) {
            out.insert(out.end(), data + offset, data + len);
            break;
        }
        int nal_len = nal_end - nal_start;
        uint8_t nal_type = data[offset + nal_start] & 0x1F;
        if (nal_type == NAL_UNIT_TYPE_SPS && nal_len > 0) {
            WriteModifiedSps(stream, data + offset + nal_start, nal_len, out);
            offset += static_cast<size_t>(nal_end);
        } else if (nal_type == NAL_UNIT_TYPE_CODED_SLICE_IDR ||
                   nal_type == NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
            out.insert(out.end(), data + offset, data + len);
            break;
        } else {
            out.insert(out.end(), data + offset, data + offset + nal_end);
            offset += static_cast<size_t>(nal_end);
        }
    }

    h264_free(stream);
    return true;
}

void Player::DidChangeView(const pp::View &view) {
    pp::Size sz = view.GetRect().size();
    if (sz.width() > 0 && sz.height() > 0) {
        viewport_size_ = sz;
        if (!context_.is_null() && sz != last_context_size_) {
            context_.ResizeBuffers(sz.width(), sz.height());
            last_context_size_ = sz;
        }
    }
}

void Player::RequestKeyframe() {
    keyframe_requested_.store(true, std::memory_order_release);
}

bool Player::SupportsHevc() {
    // Tizen 3.0 NaCl PP_VideoDecoder supports H.264 profiles up to High.
    return false;
}

bool Player::LegacyRuntime() {
    // Keep the NaCl module alive across streams without reloading the page.
    return true;
}

bool Player::LatencyNeedsReload(bool, LatencyMode) {
    return false;
}

Player::Stats Player::stats() const {
    Stats s;
    s.videoAppended = videoAppended_.load();
    s.videoFailed = videoFailed_.load();
    s.audioAppended = audioAppended_.load();
    s.audioFailed = audioFailed_.load();
    s.audioConcealed = audioConcealed_.load();
    s.audioDropped = audioDropped_.load();
    s.silenceFilled = silenceFilled_.load();
    s.audioGaps = audioGaps_.load();
    s.audioCallbackFrames = audioCallbackFrames_.load();
    s.maxFrameGapMs = maxFrameGapMs_.load();
    s.audioLeadMs = audioLeadMs_.load();
    s.videoSessionChanges = videoSessionChanges_.load();
    s.videoTrackCloses = videoTrackCloses_.load();
    s.lastCloseReason = lastCloseReason_.load();
    s.lastAppendError = lastAppendError_.load();
    s.pipelineErrors = pipelineErrors_.load();
    s.lastPipelineError = lastPipelineError_.load();
    s.picturesDecoded = picturesDecoded_.load();
    s.picturesPainted = picturesPainted_.load();
    s.picturesDropped = picturesDropped_.load();
    s.latencyModeResult = latencyModeResult_.load();
    return s;
}

int Player::VideoStart(const IHS_StreamVideoConfig &config) {
    Log(IHS_LogLevelInfo, "Player", "VideoStart: codec=%d %ux%u", (int) config.codec, config.width,
        config.height);
    if (config.codec != IHS_StreamVideoCodecH264) {
        Log(IHS_LogLevelError, "Player", "Unsupported video codec %d (NaCl requires H.264)",
            (int) config.codec);
        pipelineErrors_++;
        if (on_video_failed_) on_video_failed_(false);
        return -1;
    }

    VideoStop();

    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_queue_.clear();
        video_running_ = true;
        decoder_ready_ = false;
        decoder_failed_ = false;
        video_failed_reported_ = false;
        wait_for_keyframe_ = false;
        report_lost_needed_ = false;
        video_stop_pending_ = false;
        keyframe_requested_.store(false, std::memory_order_release);
        last_video_frame_ms_.store(0, std::memory_order_relaxed);
        maxFrameGapMs_.store(0, std::memory_order_relaxed);
    }

    setUp_ = true;
    firstKeyLogged_ = false;
    videoSessionChanges_++;

    pp::Module::Get()->core()->CallOnMainThread(
        0, video_cb_factory_.NewCallback(&Player::InitializeVideoOnMainThread, config.width,
                                         config.height));

    // Wait for the GL context and hardware H.264 decoder to initialize on the main thread
    // before telling Steam the video channel is ready, preventing a startup backlog of frames.
    bool ready = false;
    bool failed = false;
    {
        std::unique_lock<std::mutex> lock(video_mutex_);
        video_cv_.wait_for(lock, std::chrono::milliseconds(1500), [this] {
            return !video_running_ || decoder_ready_ || decoder_failed_;
        });
        ready = video_running_ && decoder_ready_ && !decoder_failed_;
        failed = decoder_failed_;
    }

    if (!ready) {
        Log(IHS_LogLevelError, "Player",
            "VideoStart aborted: decoder failed=%d timed_out=%d", (int) failed, (int) !failed);
        bool notify_failure = false;
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            if (!failed && !video_failed_reported_) {
                decoder_failed_ = true;
                video_failed_reported_ = true;
                notify_failure = true;
            }
        }
        VideoStop();
        if (notify_failure && on_video_failed_) {
            on_video_failed_(false);
        }
        return -1;
    }

    decode_thread_ = std::thread(&Player::DecodeWorkerLoop, this);
    return 0;
}

void Player::InitializeVideoOnMainThread(int32_t result, uint32_t width, uint32_t height) {
    (void) width;
    (void) height;
    if (result == PP_ERROR_ABORTED) return;
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_running_) return;
    }

    auto notifyFailure = [this]() {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            decoder_failed_ = true;
            if (!video_failed_reported_) {
                video_failed_reported_ = true;
                should_notify = true;
            }
        }
        video_cv_.notify_all();
        if (should_notify && on_video_failed_) {
            on_video_failed_(false);
        }
    };

    if (context_.is_null()) {
        if (!glInitializePPAPI(pp::Module::Get()->get_browser_interface())) {
            Log(IHS_LogLevelError, "Player", "Unable to initialize GL PPAPI");
            notifyFailure();
            return;
        }

        const int32_t attrib_list[] = {
            PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 0,
            PP_GRAPHICS3DATTRIB_BLUE_SIZE, 8,
            PP_GRAPHICS3DATTRIB_GREEN_SIZE, 8,
            PP_GRAPHICS3DATTRIB_RED_SIZE, 8,
            PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 0,
            PP_GRAPHICS3DATTRIB_STENCIL_SIZE, 0,
            PP_GRAPHICS3DATTRIB_SAMPLES, 0,
            PP_GRAPHICS3DATTRIB_SAMPLE_BUFFERS, 0,
            PP_GRAPHICS3DATTRIB_WIDTH, viewport_size_.width(),
            PP_GRAPHICS3DATTRIB_HEIGHT, viewport_size_.height(),
            PP_GRAPHICS3DATTRIB_NONE
        };

        context_ = pp::Graphics3D(instance_, attrib_list);
        if (context_.is_null() || !instance_->BindGraphics(context_)) {
            Log(IHS_LogLevelError, "Player", "Unable to create or bind Graphics3D context");
            context_ = pp::Graphics3D();
            notifyFailure();
            return;
        }

        const int32_t swap_behavior_attrib[] = {
            PP_GRAPHICS3DATTRIB_SWAP_BEHAVIOR, PP_GRAPHICS3DATTRIB_BUFFER_DESTROYED,
            PP_GRAPHICS3DATTRIB_NONE
        };
        context_.SetAttribs(swap_behavior_attrib);

        glSetCurrentContextPPAPI(context_.pp_resource());
        glDisable(GL_DITHER);
        glViewport(0, 0, viewport_size_.width(), viewport_size_.height());
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // The vertex buffer must exist and be bound BEFORE the attribute pointers are set:
        // glVertexAttribPointer captures the currently bound GL_ARRAY_BUFFER. With no buffer
        // bound, offsets 0 / 32 point into client memory (NULL) and every draw renders nothing,
        // leaving the screen black while frames are decoded fine.
        static const float kVertices[] = {
            -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,  // Position
            0.0f,  1.0f,  0.0f,  0.0f, 1.0f, 1.0f,  1.0f, 0.0f,  // Texture coords
        };

        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

        CreateShaderProgram(GL_TEXTURE_2D);
        CreateShaderProgram(GL_TEXTURE_EXTERNAL_OES);
        last_context_size_ = viewport_size_;
        gl_initialized_ = true;
        Log(IHS_LogLevelInfo, "Player", "GL surface ready %dx%d", viewport_size_.width(),
            viewport_size_.height());
    }

    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_running_) return;
    }

    if (video_decoder_) {
        delete video_decoder_;
        video_decoder_ = nullptr;
    }
    painting_ = false;
    has_next_picture_ = false;
    last_texture_target_ = 0;
    last_texture_id_ = 0;
    last_viewport_size_ = pp::Size(0, 0);
    last_texture_size_ = pp::Size(0, 0);
    video_decoder_ = new pp::VideoDecoder(instance_);
    int32_t latency_res = pp::VideoDecoderSamsung(*video_decoder_).SetLatencyMode(PP_VIDEOLATENCYMODE_LOW);
    latencyModeResult_.store(latency_res);
    Log(IHS_LogLevelInfo, "Player", "VideoDecoderSamsung::SetLatencyMode(LOW) -> %d", (int) latency_res);
    video_decoder_->Initialize(
        context_, PP_VIDEOPROFILE_H264HIGH, PP_HARDWAREACCELERATION_ONLY, 0,
        video_cb_factory_.NewCallback(&Player::OnDecoderInitialized, true));
}

void Player::OnDecoderInitialized(int32_t result, bool hwAccel) {
    if (result == PP_ERROR_ABORTED) return;

    if (result != PP_OK && hwAccel) {
        pp::VideoDecoder *decoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            if (video_running_) decoder = video_decoder_;
        }
        if (decoder) {
            Log(IHS_LogLevelWarn, "Player",
                "Hardware H.264 decoder init returned %d, falling back to software", (int) result);
            decoder->Initialize(
                context_, PP_VIDEOPROFILE_H264HIGH, PP_HARDWAREACCELERATION_NONE, 0,
                video_cb_factory_.NewCallback(&Player::OnDecoderInitialized, false));
            return;
        }
        return;
    }

    bool notify_failure = false;
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_running_) {
            video_cv_.notify_all();
            return;
        }
        if (result == PP_OK && video_decoder_) {
            decoder_ready_ = true;
            Log(IHS_LogLevelInfo, "Player", "VideoDecoder initialized (%s)",
                hwAccel ? "hardware" : "software");
            video_decoder_->GetPicture(
                video_cb_factory_.NewCallbackWithOutput(&Player::OnPictureReady));
        } else {
            Log(IHS_LogLevelError, "Player", "VideoDecoder initialization failed: %d", (int) result);
            decoder_failed_ = true;
            pipelineErrors_++;
            lastPipelineError_ = result;
            if (!video_failed_reported_) {
                video_failed_reported_ = true;
                notify_failure = true;
            }
        }
        video_cv_.notify_all();
    }

    if (notify_failure && on_video_failed_) {
        on_video_failed_(false);
    }
}

IHS_StreamVideoSubmitResult Player::VideoFrame(const uint8_t *data, size_t len, bool keyframe) {
    if (!data || len == 0) return IHS_StreamVideoSubmitOK;

    const int64_t now_ms = SteadyNowMs();
    const int64_t prev_ms = last_video_frame_ms_.exchange(now_ms, std::memory_order_relaxed);
    if (prev_ms > 0 && now_ms > prev_ms) {
        const uint32_t gap =
            static_cast<uint32_t>(std::min<int64_t>(now_ms - prev_ms, 60000));
        uint32_t cur = maxFrameGapMs_.load(std::memory_order_relaxed);
        while (gap > cur &&
               !maxFrameGapMs_.compare_exchange_weak(cur, gap, std::memory_order_relaxed)) {}
    }

    std::vector<uint8_t> unit;
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_running_ || decoder_failed_) {
            videoFailed_++;
            return IHS_StreamVideoSubmitError;
        }
        if (keyframe_requested_.exchange(false, std::memory_order_acq_rel) && !keyframe) {
            wait_for_keyframe_ = true;
            report_lost_needed_ = true;
        }
        if (keyframe) {
            wait_for_keyframe_ = false;
            report_lost_needed_ = false;
            if (!video_queue_.empty()) {
                picturesDropped_ += video_queue_.size();
                while (!video_queue_.empty()) {
                    RecycleVideoBuffer(std::move(video_queue_.front()));
                    video_queue_.pop_front();
                }
            }
        } else {
            if (report_lost_needed_) {
                report_lost_needed_ = false;
                wait_for_keyframe_ = true;
                picturesDropped_++;
                return IHS_StreamVideoSubmitReportLost;
            }
            if (wait_for_keyframe_) {
                picturesDropped_++;
                return IHS_StreamVideoSubmitOK;
            }
            if (video_queue_.size() >= kMaxQueuedVideoFrames) {
                wait_for_keyframe_ = true;
                picturesDropped_ += video_queue_.size() + 1;
                while (!video_queue_.empty()) {
                    RecycleVideoBuffer(std::move(video_queue_.front()));
                    video_queue_.pop_front();
                }
                Log(IHS_LogLevelWarn, "Player",
                    "Video decode queue stalled (%u frames), requesting keyframe",
                    static_cast<unsigned>(kMaxQueuedVideoFrames));
                return IHS_StreamVideoSubmitReportLost;
            }
        }
        unit = AcquireVideoBuffer(len + (keyframe ? 64 : 0));
    }

    PrepareH264AccessUnit(data, len, keyframe, unit);

    if (keyframe && !firstKeyLogged_.exchange(true)) {
        char in[64] = {0}, outb[64] = {0};
        for (size_t i = 0; i < 12 && i < len; ++i) std::snprintf(in + i * 3, 4, "%02x ", data[i]);
        for (size_t i = 0; i < 12 && i < unit.size(); ++i) {
            std::snprintf(outb + i * 3, 4, "%02x ", unit[i]);
        }
        Log(IHS_LogLevelInfo, "Player", "First keyframe %u bytes in: %s", (unsigned) len, in);
        Log(IHS_LogLevelInfo, "Player", "First keyframe %u bytes out: %s", (unsigned) unit.size(), outb);
    }

    std::lock_guard<std::mutex> lock(video_mutex_);
    if (!video_running_ || decoder_failed_) {
        RecycleVideoBuffer(std::move(unit));
        videoFailed_++;
        return IHS_StreamVideoSubmitError;
    }
    if (!keyframe && (wait_for_keyframe_ || video_queue_.size() >= kMaxQueuedVideoFrames)) {
        RecycleVideoBuffer(std::move(unit));
        wait_for_keyframe_ = true;
        picturesDropped_++;
        return IHS_StreamVideoSubmitReportLost;
    }
    video_queue_.push_back(std::move(unit));
    video_cv_.notify_one();
    return IHS_StreamVideoSubmitOK;
}

void Player::DecodeWorkerLoop() {
    {
        std::unique_lock<std::mutex> lock(video_mutex_);
        video_cv_.wait(lock, [this] {
            return !video_running_ || decoder_ready_ || decoder_failed_;
        });
        if (!video_running_ || decoder_failed_) return;
    }

    while (true) {
        std::vector<uint8_t> frame;
        pp::VideoDecoder *decoder = nullptr;
        {
            std::unique_lock<std::mutex> lock(video_mutex_);
            video_cv_.wait(lock, [this] {
                return !video_running_ || decoder_failed_ || !video_queue_.empty();
            });
            if (!video_running_ || decoder_failed_) break;
            frame = std::move(video_queue_.front());
            video_queue_.pop_front();
            decoder = video_decoder_;
        }
        if (!decoder || frame.empty()) {
            std::lock_guard<std::mutex> lock(video_mutex_);
            RecycleVideoBuffer(std::move(frame));
            continue;
        }

        int32_t res = decoder->Decode(next_frame_number_++, static_cast<uint32_t>(frame.size()),
                                      frame.data(), pp::BlockUntilComplete());
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            RecycleVideoBuffer(std::move(frame));
        }
        if (res == PP_OK) {
            videoAppended_++;
        } else if (res == PP_ERROR_ABORTED) {
            break;
        } else {
            videoFailed_++;
            lastAppendError_ = res;
            Log(IHS_LogLevelWarn, "Player", "VideoDecoder::Decode returned %d", (int) res);
            std::lock_guard<std::mutex> lock(video_mutex_);
            wait_for_keyframe_ = true;
            report_lost_needed_ = true;
            picturesDropped_ += video_queue_.size();
            while (!video_queue_.empty()) {
                RecycleVideoBuffer(std::move(video_queue_.front()));
                video_queue_.pop_front();
            }
        }
    }
}

void Player::ResetVideoDecoderOnMainThread(int32_t) {
    pp::VideoDecoder *decoder = nullptr;
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_running_) {
            decoder = video_decoder_;
        }
    }
    if (decoder) {
        auto noop = [](void *, int32_t) {};
        decoder->Reset(pp::CompletionCallback(noop, nullptr));
    }
}

void Player::VideoStop() {
    bool was_running = false;
    bool has_decoder = false;
    std::thread join_thread;
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (video_running_) {
            video_running_ = false;
            was_running = true;
        }
        while (!video_queue_.empty()) {
            RecycleVideoBuffer(std::move(video_queue_.front()));
            video_queue_.pop_front();
        }
        video_pool_.clear();
        join_thread = std::move(decode_thread_);
        has_decoder = (video_decoder_ != nullptr);
    }
    video_cv_.notify_all();

    const bool on_main = pp::Module::Get()->core()->IsMainThread();
    if (has_decoder) {
        if (on_main) {
            ResetVideoDecoderOnMainThread(PP_OK);
        } else {
            // Abort any blocked Decode(BlockUntilComplete) on decode_thread_ BEFORE joining it.
            pp::Module::Get()->core()->CallOnMainThread(
                0, video_cb_factory_.NewCallback(&Player::ResetVideoDecoderOnMainThread));
        }
    }

    if (join_thread.joinable()) {
        join_thread.join();
    }

    if (was_running) {
        videoTrackCloses_++;
        lastCloseReason_ = 0;
    }

    if (on_main) {
        StopVideoOnMainThread(PP_OK);
    } else if (has_decoder || was_running) {
        std::unique_lock<std::mutex> lock(video_mutex_);
        if (video_decoder_ || was_running) {
            video_stop_pending_ = true;
            pp::Module::Get()->core()->CallOnMainThread(
                0, video_cb_factory_.NewCallback(&Player::StopVideoOnMainThread));
            video_cv_.wait_for(lock, std::chrono::milliseconds(2000),
                               [this] { return video_decoder_ == nullptr && !video_stop_pending_; });
        }
    }
}

void Player::StopVideoOnMainThread(int32_t) {
    std::lock_guard<std::mutex> lock(video_mutex_);
    if (video_decoder_) {
        delete video_decoder_;
        video_decoder_ = nullptr;
    }
    video_cb_factory_.CancelAll();
    decoder_ready_ = false;
    painting_ = false;
    has_next_picture_ = false;
    video_stop_pending_ = false;
    video_cv_.notify_all();
}

void Player::OnPictureReady(int32_t result, PP_VideoPicture picture) {
    if (result == PP_ERROR_ABORTED || !video_decoder_ || !video_running_) {
        return;
    }
    if (result != PP_OK) {
        Log(IHS_LogLevelWarn, "Player", "GetPicture returned %d", (int) result);
        pipelineErrors_++;
        lastPipelineError_ = result;
        if (video_decoder_ && video_running_) {
            video_decoder_->GetPicture(
                video_cb_factory_.NewCallbackWithOutput(&Player::OnPictureReady));
        }
        return;
    }

    // Recycle bogus pictures immediately (as Moonlight does) before counting decoded frames.
    if (picture.texture_target == 0) {
        video_decoder_->RecyclePicture(picture);
        video_decoder_->GetPicture(
            video_cb_factory_.NewCallbackWithOutput(&Player::OnPictureReady));
        return;
    }
    picturesDecoded_++;
    if (picturesDecoded_ == 1) {
        Log(IHS_LogLevelInfo, "Player", "First picture decoded (texture 0x%x, %dx%d)",
            picture.texture_target, picture.texture_size.width, picture.texture_size.height);
    }

    // If the decoder produced a newer picture while SwapBuffers is still waiting on VSync,
    // recycle the unrendered older picture immediately so we always render the newest frame.
    if (has_next_picture_) {
        video_decoder_->RecyclePicture(next_picture_);
        picturesDropped_++;
    }
    next_picture_ = picture;
    has_next_picture_ = true;

    // Immediately request the next decoded picture without waiting for SwapBuffers (VSync),
    // draining the hardware decoder's internal queue as fast as frames are decoded.
    video_decoder_->GetPicture(
        video_cb_factory_.NewCallbackWithOutput(&Player::OnPictureReady));

    if (!painting_) {
        PaintNextPicture();
    }
}

void Player::PaintNextPicture() {
    if (!has_next_picture_ || !video_decoder_ || !video_running_) {
        return;
    }
    painting_ = true;
    current_picture_ = next_picture_;
    has_next_picture_ = false;
    const PP_VideoPicture &picture = current_picture_;

    Shader *shader = nullptr;
    if (picture.texture_target == GL_TEXTURE_2D) {
        shader = &prog_2d_;
    } else if (picture.texture_target == GL_TEXTURE_RECTANGLE_ARB) {
        if (!prog_rect_.program) {
            CreateShaderProgram(GL_TEXTURE_RECTANGLE_ARB);
        }
        shader = &prog_rect_;
    } else {
        shader = &prog_ext_;
    }

    if (!shader || !shader->program) {
        painting_ = false;
        video_decoder_->RecyclePicture(current_picture_);
        return;
    }

    glSetCurrentContextPPAPI(context_.pp_resource());

    const bool target_changed = (picture.texture_target != last_texture_target_);
    const pp::Size pic_size(picture.texture_size);
    if (target_changed) {
        glUseProgram(shader->program);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (shader->pos_location >= 0) {
            glEnableVertexAttribArray(shader->pos_location);
            glVertexAttribPointer(shader->pos_location, 2, GL_FLOAT, GL_FALSE, 0, 0);
        }
        if (shader->tc_location >= 0) {
            glEnableVertexAttribArray(shader->tc_location);
            glVertexAttribPointer(shader->tc_location, 2, GL_FLOAT, GL_FALSE, 0,
                                  static_cast<float *>(0) + 8);
        }
        glActiveTexture(GL_TEXTURE0);
        last_texture_target_ = picture.texture_target;
        last_texture_id_ = 0;
    }

    if (target_changed || pic_size != last_texture_size_) {
        if (picture.texture_target == GL_TEXTURE_RECTANGLE_ARB) {
            glUniform2f(shader->texcoord_scale_location, picture.texture_size.width,
                        picture.texture_size.height);
        } else {
            glUniform2f(shader->texcoord_scale_location, 1.0f, 1.0f);
        }
        last_texture_size_ = pic_size;
    }

    if (viewport_size_ != last_viewport_size_) {
        glViewport(0, 0, viewport_size_.width(), viewport_size_.height());
        last_viewport_size_ = viewport_size_;
    }

    if (picture.texture_id != last_texture_id_) {
        glBindTexture(picture.texture_target, picture.texture_id);
        last_texture_id_ = picture.texture_id;
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    context_.SwapBuffers(
        video_cb_factory_.NewCallback(&Player::OnSwapBuffersCompleted));
}

void Player::OnSwapBuffersCompleted(int32_t result) {
    if (result == PP_ERROR_ABORTED) return;
    if (!video_decoder_ || !video_running_) {
        painting_ = false;
        return;
    }
    painting_ = false;
    if (result == PP_OK) picturesPainted_++;
    video_decoder_->RecyclePicture(current_picture_);
    if (has_next_picture_) {
        PaintNextPicture();
    }
}

void Player::DeleteGL() {
    if (gl_initialized_ && !context_.is_null()) {
        glSetCurrentContextPPAPI(context_.pp_resource());
        Shader *shaders[] = {&prog_2d_, &prog_rect_, &prog_ext_};
        for (Shader *s : shaders) {
            if (s->program) {
                glDeleteProgram(s->program);
                s->program = 0;
            }
            s->pos_location = -1;
            s->tc_location = -1;
            s->texcoord_scale_location = 0;
        }
        if (vbo_) {
            glDeleteBuffers(1, &vbo_);
            vbo_ = 0;
        }
        glSetCurrentContextPPAPI(0);
    }
    gl_initialized_ = false;
    last_texture_target_ = 0;
    last_texture_id_ = 0;
    last_texture_size_ = pp::Size(0, 0);
}

void Player::CreateShaderProgram(int textureType) {
    Shader *shader = nullptr;
    const char *fragment_source = nullptr;

    static const char kVertexShader[] =
        "varying vec2 v_texCoord;            \n"
        "attribute vec4 a_position;          \n"
        "attribute vec2 a_texCoord;          \n"
        "uniform vec2 v_scale;               \n"
        "void main()                         \n"
        "{                                   \n"
        "    v_texCoord = v_scale * a_texCoord; \n"
        "    gl_Position = a_position;       \n"
        "}";

    if (textureType == GL_TEXTURE_2D) {
        shader = &prog_2d_;
        fragment_source =
            "precision lowp float;               \n"
            "varying vec2 v_texCoord;            \n"
            "uniform sampler2D s_texture;        \n"
            "void main()                         \n"
            "{"
            "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
            "}";
    } else if (textureType == GL_TEXTURE_RECTANGLE_ARB) {
        shader = &prog_rect_;
        fragment_source =
            "#extension GL_ARB_texture_rectangle : require\n"
            "precision lowp float;               \n"
            "varying vec2 v_texCoord;            \n"
            "uniform sampler2DRect s_texture;    \n"
            "void main()                         \n"
            "{"
            "    gl_FragColor = texture2DRect(s_texture, v_texCoord).rgba; \n"
            "}";
    } else {
        shader = &prog_ext_;
        fragment_source =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision lowp float;               \n"
            "varying vec2 v_texCoord;            \n"
            "uniform samplerExternalOES s_texture; \n"
            "void main()                         \n"
            "{"
            "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
            "}";
    }

    shader->program = glCreateProgram();
    if (!shader->program) {
        Log(IHS_LogLevelError, "Player", "glCreateProgram failed for texture 0x%x", textureType);
        return;
    }
    CreateShader(shader->program, GL_VERTEX_SHADER, kVertexShader, sizeof(kVertexShader));
    CreateShader(shader->program, GL_FRAGMENT_SHADER, fragment_source,
                 static_cast<int>(std::strlen(fragment_source) + 1));
    glLinkProgram(shader->program);

    GLint linked = 0;
    glGetProgramiv(shader->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        Log(IHS_LogLevelWarn, "Player", "Shader program for texture 0x%x did not link", textureType);
        glDeleteProgram(shader->program);
        shader->program = 0;
        return;
    }

    glUseProgram(shader->program);
    glUniform1i(glGetUniformLocation(shader->program, "s_texture"), 0);
    shader->texcoord_scale_location = glGetUniformLocation(shader->program, "v_scale");

    GLint pos_location = glGetAttribLocation(shader->program, "a_position");
    GLint tc_location = glGetAttribLocation(shader->program, "a_texCoord");
    if (pos_location < 0 || tc_location < 0) {
        Log(IHS_LogLevelError, "Player", "Shader 0x%x missing attrs (pos=%d tc=%d)",
            textureType, pos_location, tc_location);
        glUseProgram(0);
        glDeleteProgram(shader->program);
        shader->program = 0;
        return;
    }
    shader->pos_location = pos_location;
    shader->tc_location = tc_location;
    glEnableVertexAttribArray(pos_location);
    glVertexAttribPointer(pos_location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(tc_location);
    glVertexAttribPointer(tc_location, 2, GL_FLOAT, GL_FALSE, 0,
                          static_cast<float *>(0) + 8);
    glUseProgram(0);
}

void Player::CreateShader(uint32_t program, uint32_t type, const char *source, int size) {
    GLuint shader = glCreateShader(type);
    GLint length = size - 1;
    glShaderSource(shader, 1, &source, &length);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glDeleteShader(shader);
}

// Audio implementation (Opus -> 48 kHz 16-bit stereo -> pp::Audio)

int Player::AudioStart(const IHS_StreamAudioConfig &config) {
    Log(IHS_LogLevelInfo, "Player", "AudioStart: codec=%d freq=%u ch=%u", (int) config.codec,
        config.frequency, config.channels);
    if (config.codec != IHS_StreamAudioCodecOpus) {
        Log(IHS_LogLevelWarn, "Player", "Non-Opus audio codec %d is not supported",
            (int) config.codec);
        return -1;
    }

    AudioStop();

    const uint32_t channels = (config.channels == 1) ? 1u : 2u;
    // Opus internal codec sample rate is 48 kHz (libopus rejects 44.1 kHz); decoding at 48 kHz
    // handles any valid Opus payload cleanly and matches PP_AUDIOSAMPLERATE_48000.
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(kAudioSampleRate, static_cast<int>(channels), &err);
    if (err != OPUS_OK || !dec) {
        Log(IHS_LogLevelError, "Player", "opus_decoder_create(48000, %uch) failed: %d",
            channels, err);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(opus_mutex_);
        opus_ = dec;
        audio_channels_ = channels;
        last_opus_frames_ = 480;
        decode_pcm_.resize(5760 * kAudioChannels);
    }

    {
        std::unique_lock<std::mutex> lock(audio_mutex_);
        if (audio_ring_.size() != kAudioRingCapacity * kAudioChannels) {
            audio_ring_.assign(kAudioRingCapacity * kAudioChannels, 0);
        }
        audio_read_frame_.store(0, std::memory_order_relaxed);
        audio_write_frame_.store(0, std::memory_order_relaxed);
        audio_seen_packet_.store(false, std::memory_order_relaxed);
        audio_ring_active_.store(false, std::memory_order_release);
        audioLeadMs_.store(0, std::memory_order_relaxed);
        silenceFilled_.store(0, std::memory_order_relaxed);
        audioGaps_.store(0, std::memory_order_relaxed);
        audio_running_ = true;
        audio_player_ready_ = false;
        audio_stop_pending_ = false;
        if (pp::Module::Get()->core()->IsMainThread()) {
            lock.unlock();
            StartAudioOnMainThread(PP_OK);
        } else {
            pp::Module::Get()->core()->CallOnMainThread(
                0, audio_cb_factory_.NewCallback(&Player::StartAudioOnMainThread));
            audio_cv_.wait_for(lock, std::chrono::milliseconds(2000),
                               [this] { return audio_player_ready_ || !audio_running_; });
        }
    }
    return 0;
}

void Player::StartAudioOnMainThread(int32_t result) {
    if (result == PP_ERROR_ABORTED) return;

    pp::Audio old_player;
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (!audio_running_) {
            audio_player_ready_ = true;
            audio_cv_.notify_all();
            return;
        }
        old_player = audio_player_;
        audio_player_ = pp::Audio();
        audio_ring_active_.store(false, std::memory_order_release);
    }
    if (!old_player.is_null()) {
        old_player.StopPlayback();
    }

    pp::AudioConfig cfg(instance_, PP_AUDIOSAMPLERATE_48000, kAudioCallbackFrames);
    uint32_t actual = cfg.sample_frame_count();
    if (actual == 0) actual = kAudioCallbackFrames;

    const size_t max_target =
        std::min<size_t>(kAudioRingCapacity - 1024, std::max<size_t>(1440, static_cast<size_t>(actual) * 2 + 960));
    const size_t trim_target =
        std::min<size_t>(max_target / 2, std::max<size_t>(480, static_cast<size_t>(actual) + 480));

    audioCallbackFrames_.store(actual, std::memory_order_relaxed);
    audio_trim_target_frames_.store(trim_target, std::memory_order_relaxed);
    audio_max_target_frames_.store(max_target, std::memory_order_relaxed);
    audio_read_frame_.store(0, std::memory_order_relaxed);
    audio_write_frame_.store(0, std::memory_order_relaxed);
    audio_seen_packet_.store(false, std::memory_order_relaxed);

    pp::Audio player(instance_, cfg, AudioPlayerSampleCallback, this);
    audio_ring_active_.store(true, std::memory_order_release);
    const bool started = !player.is_null() && player.StartPlayback();

    Log(IHS_LogLevelInfo, "Player",
        "Audio playback %s (requested %u, actual %u frames, trim %u, max %u @ 48kHz)",
        started ? "started" : "failed to start", kAudioCallbackFrames,
        static_cast<unsigned>(actual), static_cast<unsigned>(trim_target),
        static_cast<unsigned>(max_target));

    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (started && audio_running_) {
            audio_player_ = player;
            audio_player_ready_ = true;
            audio_cv_.notify_all();
            return;
        }
        audio_ring_active_.store(false, std::memory_order_release);
        audio_player_ready_ = true;
    }
    if (started && !player.is_null()) {
        player.StopPlayback();
    }
    audio_cv_.notify_all();
}

void Player::PushAudioFrames(const int16_t *pcm, size_t frames) {
    if (!pcm || frames == 0 || !audio_ring_active_.load(std::memory_order_acquire)) return;

    const size_t r = audio_read_frame_.load(std::memory_order_acquire);
    const size_t w = audio_write_frame_.load(std::memory_order_relaxed);
    const size_t used = w - r;
    const size_t space = (used < kAudioRingCapacity) ? (kAudioRingCapacity - used) : 0;

    if (frames > space) {
        audioDropped_.fetch_add(frames - space, std::memory_order_relaxed);
        frames = space;
        if (frames == 0) return;
    }

    const size_t w_idx = w & kAudioRingMask;
    const size_t first_frames = std::min(frames, kAudioRingCapacity - w_idx);
    std::memcpy(&audio_ring_[w_idx * kAudioChannels], pcm,
                first_frames * kAudioChannels * sizeof(int16_t));
    const size_t rem_frames = frames - first_frames;
    if (rem_frames > 0) {
        std::memcpy(&audio_ring_[0], pcm + first_frames * kAudioChannels,
                    rem_frames * kAudioChannels * sizeof(int16_t));
    }

    audio_write_frame_.store(w + frames, std::memory_order_release);
    audioLeadMs_.store(static_cast<int64_t>((used + frames) * 1000 / kAudioSampleRate),
                       std::memory_order_relaxed);
}

int Player::AudioPacket(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0;

    int decoded_samples = 0;
    const int16_t *pcm_out = nullptr;
    std::lock_guard<std::mutex> lock(opus_mutex_);
    if (!opus_) return 0;

    if (decode_pcm_.size() < 5760 * kAudioChannels) {
        decode_pcm_.resize(5760 * kAudioChannels);
    }
    decoded_samples = opus_decode(opus_, data, static_cast<opus_int32>(len), decode_pcm_.data(),
                                  5760, 0);
    if (decoded_samples <= 0) {
        audioFailed_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    last_opus_frames_ = static_cast<uint32_t>(decoded_samples);

    if (audio_channels_ == 1) {
        // Upmix mono in-place from back to front into interleaved stereo.
        for (int i = decoded_samples - 1; i >= 0; --i) {
            const int16_t s = decode_pcm_[static_cast<size_t>(i)];
            decode_pcm_[static_cast<size_t>(i) * 2] = s;
            decode_pcm_[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }
    pcm_out = decode_pcm_.data();
    PushAudioFrames(pcm_out, static_cast<size_t>(decoded_samples));
    audioAppended_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void Player::AudioLost(uint32_t frames) {
    if (frames == 0 || !audio_ring_active_.load(std::memory_order_acquire)) return;
    if (frames > 4) frames = 4;

    const size_t trim_target = audio_trim_target_frames_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i) {
        const size_t r = audio_read_frame_.load(std::memory_order_acquire);
        const size_t w = audio_write_frame_.load(std::memory_order_relaxed);
        if ((w - r) >= trim_target) break;

        std::lock_guard<std::mutex> lock(opus_mutex_);
        if (!opus_) break;
        const int plc_frames = static_cast<int>(
            std::min<uint32_t>(5760, std::max<uint32_t>(120, last_opus_frames_)));
        if (decode_pcm_.size() < static_cast<size_t>(plc_frames) * kAudioChannels) {
            decode_pcm_.resize(static_cast<size_t>(plc_frames) * kAudioChannels);
        }
        int decoded = opus_decode(opus_, nullptr, 0, decode_pcm_.data(), plc_frames, 0);
        if (decoded > 0) {
            if (audio_channels_ == 1) {
                for (int s_idx = decoded - 1; s_idx >= 0; --s_idx) {
                    const int16_t s = decode_pcm_[static_cast<size_t>(s_idx)];
                    decode_pcm_[static_cast<size_t>(s_idx) * 2] = s;
                    decode_pcm_[static_cast<size_t>(s_idx) * 2 + 1] = s;
                }
            }
            PushAudioFrames(decode_pcm_.data(), static_cast<size_t>(decoded));
            audioConcealed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Player::AudioStop() {
    const bool on_main = pp::Module::Get()->core()->IsMainThread();
    bool need_main_stop = false;
    {
        std::unique_lock<std::mutex> lock(audio_mutex_);
        const bool was_running = audio_running_;
        audio_running_ = false;
        audio_ring_active_.store(false, std::memory_order_release);
        need_main_stop = was_running || !audio_player_.is_null();
        if (!on_main && need_main_stop) {
            audio_stop_pending_ = true;
            pp::Module::Get()->core()->CallOnMainThread(
                0, audio_cb_factory_.NewCallback(&Player::StopAudioOnMainThread));
            audio_cv_.wait_for(lock, std::chrono::milliseconds(2000),
                               [this] { return !audio_stop_pending_; });
        }
    }

    if (on_main && need_main_stop) {
        StopAudioOnMainThread(PP_OK);
    }

    {
        std::lock_guard<std::mutex> lock(opus_mutex_);
        if (opus_) {
            opus_decoder_destroy(opus_);
            opus_ = nullptr;
        }
    }

    audio_read_frame_.store(0, std::memory_order_relaxed);
    audio_write_frame_.store(0, std::memory_order_relaxed);
    audio_seen_packet_.store(false, std::memory_order_relaxed);
    audioLeadMs_.store(0, std::memory_order_relaxed);
}

void Player::StopAudioOnMainThread(int32_t) {
    pp::Audio old_player;
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        old_player = audio_player_;
        audio_player_ = pp::Audio();
        audio_ring_active_.store(false, std::memory_order_release);
    }
    if (!old_player.is_null()) {
        old_player.StopPlayback();
    }
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_cb_factory_.CancelAll();
        audio_player_ready_ = false;
        audio_stop_pending_ = false;
    }
    audio_cv_.notify_all();
}

void Player::AudioPlayerSampleCallback(void *samples, uint32_t buffer_size, void *data) {
    auto *self = static_cast<Player *>(data);
    uint32_t sample_count = buffer_size / sizeof(int16_t);
    self->FillAudioSamples(static_cast<int16_t *>(samples), sample_count);
}

void Player::FillAudioSamples(int16_t *out, uint32_t sample_count) {
    const size_t req_frames = sample_count / kAudioChannels;
    if (!audio_ring_active_.load(std::memory_order_acquire) || req_frames == 0) {
        std::memset(out, 0, sample_count * sizeof(int16_t));
        return;
    }

    const size_t w = audio_write_frame_.load(std::memory_order_acquire);
    size_t r = audio_read_frame_.load(std::memory_order_relaxed);
    size_t avail = w - r;

    const size_t max_target = audio_max_target_frames_.load(std::memory_order_relaxed);
    const size_t trim_target = audio_trim_target_frames_.load(std::memory_order_relaxed);
    const size_t keep = std::max(trim_target, req_frames);
    if (avail > max_target && avail > keep) {
        const size_t drop = avail - keep;
        r += drop;
        avail -= drop;
        audioDropped_.fetch_add(drop, std::memory_order_relaxed);
    }

    const size_t take_frames = std::min(req_frames, avail);
    if (take_frames > 0) {
        const size_t r_idx = r & kAudioRingMask;
        const size_t first_frames = std::min(take_frames, kAudioRingCapacity - r_idx);
        std::memcpy(out, &audio_ring_[r_idx * kAudioChannels],
                    first_frames * kAudioChannels * sizeof(int16_t));
        const size_t rem_frames = take_frames - first_frames;
        if (rem_frames > 0) {
            std::memcpy(out + first_frames * kAudioChannels, &audio_ring_[0],
                        rem_frames * kAudioChannels * sizeof(int16_t));
        }
        r += take_frames;
        audio_seen_packet_.store(true, std::memory_order_relaxed);
    }
    audio_read_frame_.store(r, std::memory_order_release);

    const size_t copied_samples = take_frames * kAudioChannels;
    if (copied_samples < sample_count) {
        std::memset(out + copied_samples, 0, (sample_count - copied_samples) * sizeof(int16_t));
        if (audio_seen_packet_.load(std::memory_order_relaxed)) {
            silenceFilled_.fetch_add(req_frames - take_frames, std::memory_order_relaxed);
            audioGaps_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    audioLeadMs_.store(static_cast<int64_t>((avail - take_frames) * 1000 / kAudioSampleRate),
                       std::memory_order_relaxed);
}

}  // namespace tsl
