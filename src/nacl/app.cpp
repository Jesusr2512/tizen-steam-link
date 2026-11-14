#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "nacl_io/nacl_io.h"
#include "ppapi/c/ppb_gamepad.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_array.h"
#include "ppapi/cpp/var_dictionary.h"
#include "ppapi/cpp/view.h"
#include "ppapi/utility/completion_callback_factory.h"

#include "core/client.h"
#include "core/compat.h"
#include "core/gamepad.h"
#include "core/identity.h"
#include "core/keymap.h"
#include "core/log.h"
#include "core/session.h"
#include "nacl/player.h"

namespace tsl {
namespace {

std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string Quote(const std::string &s) { return "\"" + JsonEscape(s) + "\""; }

std::string HostJson(const Host &host) {
    return "{\"ip\":" + Quote(host.address()) + ",\"hostname\":" + Quote(host.hostname()) +
           ",\"ostype\":" + std::to_string(host.info.ostype) + ",\"universe\":" +
           std::to_string(host.info.universe) + ",\"gamesRunning\":" +
           (host.info.gamesRunning ? "true" : "false") + ",\"mac\":" + Quote(host.macAddress()) + "}";
}

// A single thread that runs posted jobs in order.
class Worker {
public:
    Worker() : thread_([this] { Run(); }) {}
    ~Worker() { Stop(); }
    void Stop() {
        if (!thread_.joinable()) return;
        Post(nullptr);
        thread_.join();
    }
    void Post(std::function<void()> job) {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        cond_.notify_one();
    }

private:
    void Run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            if (!job) return;
            job();
        }
    }
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
};

struct StreamParams {
    uint64_t generation = 0;
    std::string ip;
    uint64_t steamId = 0;
    HostKey key{};
    StreamRequest request;
    SessionOptions options;
};

struct RawPadSample {
    bool connected = false;
    char id[128] = {};
    size_t numAxes = 0;
    double axes[16] = {};
    size_t numButtons = 0;
    double buttons[64] = {};
    bool pressed[64] = {};
};

class TslInstance : public pp::Instance,
                    public Client::Listener,
                    public Session::Listener {
public:
    explicit TslInstance(PP_Instance instance)
        : pp::Instance(instance), callback_factory_(this) {
        std::lock_guard<std::mutex> lock(s_instanceMutex_);
        s_instance_ = this;
    }

    ~TslInstance() override {
        SetLogSink(nullptr);
        {
            std::lock_guard<std::mutex> lock(s_instanceMutex_);
            if (s_instance_ == this) {
                s_instance_ = nullptr;
            }
        }
        polling_ = false;
        streaming_ = false;
        if (gamepadThread_.joinable()) {
            gamepadThread_.join();
        }
        // Stop Pepper video/audio directly on Main Thread before stopping worker/session
        // so Session::Join() never waits on CallOnMainThread while ~TslInstance blocks Main Thread.
        if (player_) {
            player_->VideoStop();
            player_->AudioStop();
        }
        if (auto session = CurrentSession()) {
            session->Disconnect();
        }
        worker_.Stop();
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            session_.reset();
        }
        client_.reset();
        player_.reset();
    }

    bool Init(uint32_t argc, const char *argn[], const char *argv[]) override {
        (void) argc;
        (void) argn;
        (void) argv;
        nacl_io_init_ppapi(pp_instance(), pp::Module::Get()->get_browser_interface());
        gamepad_api_ = static_cast<const PPB_Gamepad *>(
            pp::Module::Get()->GetBrowserInterface(PPB_GAMEPAD_INTERFACE));

        SetLogSink(&TslInstance::OnLogStatic);
        SetLogLevel(IHS_LogLevelInfo);
        IHS_Init();

        player_.reset(new Player(this, Player::LatencyMode::Low, [this](bool restartNeeded) {
            PostEvent("videoFailed",
                      std::string("{\"restartNeeded\":") + (restartNeeded ? "true" : "false") + "}");
        }));

        // Notify bridge.js that the NaCl module is loaded and ready.
        PostEvent("ready", "{}");
        return true;
    }

    void DidChangeView(const pp::View &view) override {
        if (player_) {
            player_->DidChangeView(view);
        }
    }

    void DidChangeFocus(bool has_focus) override {
        if (has_focus && streaming_.load(std::memory_order_acquire) && player_) {
            player_->RequestKeyframe();
        }
    }

    void HandleMessage(const pp::Var &var_message) override {
        if (!var_message.is_dictionary()) return;
        pp::VarDictionary dict(var_message);
        std::string cmd = dict.Get("cmd").AsString();

        if (cmd == "gamepadSample") {
            HandleJsGamepadSample(dict);
            return;
        }

        DispatchCommand(cmd, dict);
    }

private:
    static std::mutex s_instanceMutex_;
    static TslInstance *s_instance_;

    static void OnLogStatic(IHS_LogLevel level, const char *tag, const char *message) {
        std::printf("[%s %s] %s\n", IHS_LogLevelName(level), tag ? tag : "", message ? message : "");
        std::lock_guard<std::mutex> lock(s_instanceMutex_);
        if (s_instance_ && level <= IHS_LogLevelInfo) {
            s_instance_->PostEvent("log", "{\"level\":" + std::to_string(level) + ",\"tag\":" +
                                                  Quote(tag ? tag : "") + ",\"message\":" +
                                                  Quote(message ? message : "") + "}");
        }
    }

    static void OnRumbleStatic(int slot, uint16_t low, uint16_t high, uint32_t durationMs, void *) {
        std::lock_guard<std::mutex> lock(s_instanceMutex_);
        if (s_instance_) {
            s_instance_->PostEvent("rumble", "{\"slot\":" + std::to_string(slot) + ",\"low\":" +
                                                     std::to_string(low) + ",\"high\":" +
                                                     std::to_string(high) + ",\"ms\":" +
                                                     std::to_string(durationMs) + "}");
        }
    }

    void PostEvent(const std::string &type, const std::string &json) {
        EnqueueOutbox(type, json);
    }

    void PostStats(const std::string &json) {
        EnqueueOutbox("stats", json);
    }

    void EnqueueOutbox(const std::string &type, const std::string &json) {
        bool needSchedule = false;
        {
            std::lock_guard<std::mutex> lock(outbox_mutex_);
            outbox_.emplace_back(type, json);
            if (!outbox_scheduled_) {
                outbox_scheduled_ = true;
                needSchedule = true;
            }
        }
        if (needSchedule && pp::Module::Get() && pp::Module::Get()->core()) {
            pp::Module::Get()->core()->CallOnMainThread(
                0, callback_factory_.NewCallback(&TslInstance::FlushOutboxOnMainThread));
        }
    }

    void FlushOutboxOnMainThread(int32_t) {
        std::deque<std::pair<std::string, std::string>> batch;
        {
            std::lock_guard<std::mutex> lock(outbox_mutex_);
            batch.swap(outbox_);
            outbox_scheduled_ = false;
        }
        for (const auto &item : batch) {
            pp::VarDictionary msg;
            msg.Set("event", item.first);
            msg.Set("data", item.second);
            PostMessage(msg);
        }
    }

    void DispatchCommand(const std::string &cmd, const pp::VarDictionary &dict) {
        if (cmd == "init") {
            InitClient(dict.Get("identity").AsString());
        } else if (cmd == "startDiscovery") {
            StartDiscovery();
        } else if (cmd == "stopDiscovery") {
            StopDiscovery();
        } else if (cmd == "probe") {
            Probe(dict.Get("ip").AsString());
        } else if (cmd == "wake") {
            Wake(dict.Get("mac").AsString());
        } else if (cmd == "pair") {
            Pair(dict.Get("ip").AsString(), dict.Get("pin").AsString());
        } else if (cmd == "cancelPair") {
            CancelPair();
        } else if (cmd == "startStream") {
            StartStream(
                dict.Get("ip").AsString(),
                dict.Get("steamId").AsString(),
                dict.Get("hostKeyHex").AsString(),
                dict.Get("width").AsInt(),
                dict.Get("height").AsInt(),
                dict.Get("hevc").AsBool(),
                dict.Get("audio").AsBool(),
                dict.Get("bigPicture").AsBool(),
                dict.Get("quality").AsInt(),
                dict.Get("bitrateKbps").AsInt(),
                dict.Get("framerate").AsInt(),
                dict.Get("hdr").AsBool(),
                dict.Get("keepPlaying").AsBool());
        } else if (cmd == "stopStream") {
            StopStream();
        } else if (cmd == "key") {
            Key(dict.Get("code").AsString(), dict.Get("down").AsBool());
        } else if (cmd == "sendText") {
            SendText(dict.Get("text").AsString());
        } else if (cmd == "setPadInput") {
            SetPadInput(dict.Get("enabled").AsBool());
        } else if (cmd == "mouseMove") {
            MouseMove(dict.Get("dx").AsInt(), dict.Get("dy").AsInt());
        } else if (cmd == "mouseButton") {
            MouseButton(dict.Get("jsButton").AsInt(), dict.Get("down").AsBool());
        } else if (cmd == "mouseWheel") {
            MouseWheel(dict.Get("deltaY").AsInt());
        } else if (cmd == "stats") {
            PostStats(Stats());
        } else if (cmd == "setLogLevel") {
            SetLogLevel(static_cast<IHS_LogLevel>(dict.Get("level").AsInt()));
        }
    }

    bool InitClient(const std::string &serialized) {
        if (client_) return true;
        auto identity = Identity::Parse(serialized);
        if (!identity) return false;
        identity_ = *identity;
        client_.reset(new Client(identity_, this));
        polling_ = true;
        gamepadThread_ = std::thread([this] { PollGamepads(); });
        return true;
    }

    void StartDiscovery() {
        if (client_) worker_.Post([this] { client_->StartDiscovery(2000); });
    }

    void StopDiscovery() {
        if (client_) worker_.Post([this] { client_->StopDiscovery(); });
    }

    bool Probe(const std::string &ip) {
        IHS_IPAddress address{};
        if (!client_ || !IHS_IPAddressFromString(&address, ip.c_str())) return false;
        worker_.Post([this, ip] { client_->Probe(ip); });
        return true;
    }

    bool Wake(const std::string &mac) {
        if (!client_ || !WakePacket(mac)) return false;
        worker_.Post([this, mac] { client_->Wake(mac); });
        return true;
    }

    bool Pair(const std::string &ip, const std::string &pin) {
        if (!client_) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pairActive_) return false;
            pendingPair_ = ip;
            pendingPin_ = pin;
        }
        return Probe(ip);
    }

    void CancelPair() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingPair_.clear();
        }
        worker_.Post([this] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pairActive_) return;
                pairActive_ = false;
            }
            client_->CancelPair();
        });
    }

    bool StartStream(const std::string &ip, const std::string &steamId, const std::string &keyHex,
                     int maxWidth, int maxHeight, bool hevc, bool audio, bool bigPicture,
                     int quality, int bitrateKbps, int fps, bool hdr, bool keepPlaying) {
        if (!client_) return false;
        StreamParams params;
        params.ip = ip;
        params.steamId = std::strtoull(steamId.c_str(), nullptr, 10);
        if (!HexDecode(keyHex, params.key.data(), params.key.size())) return false;
        params.request.maxWidth = maxWidth;
        params.request.maxHeight = maxHeight;
        params.request.interface =
            bigPicture ? IHS_StreamInterfaceBigPicture : IHS_StreamInterfaceDesktop;
        params.request.hdr = false;
        (void) hevc;
        (void) hdr;
        params.options.hevc = false;
        params.options.hdr = false;
        params.options.audio = audio;
        params.options.quality = quality;
        if (bitrateKbps == -2) {
            params.options.unlimitedBitrate = true;
            params.options.bitrateKbps = 0;
        } else {
            params.options.unlimitedBitrate = false;
            params.options.bitrateKbps = bitrateKbps;
        }
        params.options.desktopMode = !bigPicture;
        params.options.maxWidth = maxWidth;
        params.options.maxHeight = maxHeight;
        params.options.maxFramerate = fps > 0 ? fps : 60;
        params.options.keepPlayingAfterLoss = keepPlaying;

        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_) return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingStream_) return false;
            params.generation = ++streamGeneration_;
            pendingStream_ = params;
            streamRequested_ = false;
        }
        return Probe(params.ip);
    }

    void StopStream() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingStream_.reset();
            streamRequested_ = false;
        }
        if (auto session = CurrentSession()) {
            session->Disconnect();
        }
    }

    void Key(const std::string &code, bool down) {
        uint32_t scancode = ScancodeFromDomCode(code);
        if (scancode == 0) return;
        if (auto session = CurrentSession()) session->SendKey(scancode, down);
    }

    void MouseMove(int dx, int dy) {
        if (auto session = CurrentSession()) session->SendMouseMove(dx, dy);
    }

    void SendText(const std::string &text) {
        if (auto session = CurrentSession()) session->SendText(text.c_str());
    }

    void SetPadInput(bool enabled) { padInput_ = enabled; }

    void MouseButton(int domButton, bool down) {
        IHS_StreamInputMouseButton button;
        switch (domButton) {
            case 0: button = IHS_MOUSE_BUTTON_LEFT; break;
            case 1: button = IHS_MOUSE_BUTTON_MIDDLE; break;
            case 2: button = IHS_MOUSE_BUTTON_RIGHT; break;
            case 3: button = IHS_MOUSE_BUTTON_X1; break;
            case 4: button = IHS_MOUSE_BUTTON_X2; break;
            default: return;
        }
        if (auto session = CurrentSession()) session->SendMouseButton(button, down);
    }

    void MouseWheel(int deltaY) {
        if (deltaY == 0) return;
        if (auto session = CurrentSession()) {
            session->SendMouseWheel(deltaY < 0 ? IHS_MOUSE_WHEEL_UP : IHS_MOUSE_WHEEL_DOWN);
        }
    }

    std::string Stats() {
        SessionStats s;
        bool active = false;
        if (auto session = CurrentSession()) {
            s = session->Stats();
            active = session->connected();
        }
        Player::Stats c = player_ ? player_->stats() : Player::Stats{};
        return "{\"connected\":" + std::string(active ? "true" : "false") +
               ",\"videoFrames\":" + std::to_string(s.videoFrames) + ",\"keyFrames\":" +
               std::to_string(s.keyFrames) + ",\"videoBytes\":" + std::to_string(s.videoBytes) +
               ",\"audioPackets\":" + std::to_string(s.audioPackets) + ",\"width\":" +
               std::to_string(s.width) + ",\"height\":" + std::to_string(s.height) +
               ",\"videoCodec\":" + std::to_string(s.videoCodec) + ",\"colorspace\":" +
               std::to_string(s.colorspace) + ",\"targetBitrate\":" +
               std::to_string(s.targetBitrate) + ",\"framerateNum\":" +
               std::to_string(s.framerateNum) + ",\"framerateDen\":" +
               std::to_string(s.framerateDen) + ",\"videoAppended\":" +
               std::to_string(c.videoAppended) + ",\"videoFailed\":" +
               std::to_string(c.videoFailed) + ",\"audioAppended\":" +
               std::to_string(c.audioAppended) + ",\"audioFailed\":" +
               std::to_string(c.audioFailed) + ",\"silenceFilled\":" +
               std::to_string(c.silenceFilled) + ",\"audioDropped\":" +
               std::to_string(c.audioDropped) + ",\"audioCallbackFrames\":" +
               std::to_string(c.audioCallbackFrames) + ",\"ppbGamepads\":" +
               std::to_string(ppbGamepadMask_.load(std::memory_order_relaxed)) +
               ",\"videoPackets\":" + std::to_string(s.videoPackets) + ",\"videoPacketsLost\":" +
               std::to_string(s.videoPacketsLost) + ",\"lostReports\":" +
               std::to_string(s.lostReports) + ",\"roundTripMs\":" +
               std::to_string(s.roundTripMs) + ",\"fec\":" + (s.fec ? "true" : "false") +
               ",\"fecRecovered\":" + std::to_string(s.fecRecovered) + ",\"fecUnrecoverable\":" +
               std::to_string(s.fecUnrecoverable) + ",\"videoSessionChanges\":" +
               std::to_string(c.videoSessionChanges) + ",\"videoTrackCloses\":" +
               std::to_string(c.videoTrackCloses) + ",\"lastCloseReason\":" +
               std::to_string(c.lastCloseReason) + ",\"lastAppendError\":" +
               std::to_string(c.lastAppendError) + ",\"pipelineErrors\":" +
               std::to_string(c.pipelineErrors) + ",\"lastPipelineError\":" +
               std::to_string(c.lastPipelineError) + ",\"audioConcealed\":" +
               std::to_string(c.audioConcealed) + ",\"picturesDecoded\":" +
               std::to_string(c.picturesDecoded) + ",\"picturesPainted\":" +
               std::to_string(c.picturesPainted) + ",\"picturesDropped\":" +
               std::to_string(c.picturesDropped) + ",\"audioGaps\":" +
               std::to_string(c.audioGaps) + ",\"audioLeadMs\":" +
               std::to_string(c.audioLeadMs) + ",\"maxFrameGapMs\":" +
               std::to_string(c.maxFrameGapMs) + ",\"latencyModeResult\":" +
               std::to_string(c.latencyModeResult) + "}";
    }

    // Client::Listener (ihslib client thread)

    void OnHostFound(const Host &host) override {
        std::string ip = host.address();
        bool pair = false, stream = false;
        std::string pin;
        StreamParams params;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hosts_[ip] = host;
            if (pendingPair_ == ip) {
                pair = true;
                pin = pendingPin_;
                pendingPair_.clear();
            }
            if (pendingStream_ && pendingStream_->ip == ip && !streamRequested_) {
                stream = true;
                streamRequested_ = true;
                params = *pendingStream_;
            }
        }
        PostEvent("hostFound", HostJson(host));
        if (pair) {
            worker_.Post([this, host, pin] {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (pairActive_) return;
                    pairActive_ = true;
                }
                if (!client_->Pair(host, pin)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        pairActive_ = false;
                    }
                    PostEvent("pairFailed", "{\"ip\":" + Quote(host.address()) +
                                                    ",\"reason\":\"could not start pairing\"}");
                }
            });
        }
        if (stream) {
            worker_.Post([this, host, params] {
                if (!client_->RequestStream(host, params.request, params.key)) {
                    FailStream("could not send the streaming request");
                }
            });
        }
    }

    void OnPairProgress(const Host &host) override {
        PostEvent("pairProgress", "{\"ip\":" + Quote(host.address()) + "}");
    }

    void OnPairSucceeded(const Host &host, uint64_t steamId, const HostKey &key) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pairActive_ = false;
        }
        PostEvent("pairSucceeded", "{\"ip\":" + Quote(host.address()) + ",\"hostname\":" +
                                           Quote(host.hostname()) + ",\"steamId\":\"" +
                                           std::to_string(steamId) + "\",\"key\":\"" +
                                           HexEncode(key.data(), key.size()) + "\"}");
    }

    void OnPairFailed(const Host &host, IHS_AuthorizationResult result) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pairActive_ = false;
        }
        PostEvent("pairFailed", "{\"ip\":" + Quote(host.address()) + ",\"result\":" +
                                        std::to_string(result) + ",\"reason\":" +
                                        Quote(AuthorizationResultName(result)) + "}");
    }

    void OnStreamProgress(const Host &host) override {
        PostEvent("streamProgress", "{\"ip\":" + Quote(host.address()) + "}");
    }

    void OnStreamReady(const Host &host, const IHS_SessionInfo &info) override {
        std::string ip = host.address();
        worker_.Post([this, info, ip] {
            StreamParams params;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pendingStream_ || pendingStream_->ip != ip) return;
                params = *pendingStream_;
            }
            IHS_SessionInfo sessionInfo = info;
            sessionInfo.steamId = params.steamId;
            auto session = std::make_shared<Session>(
                identity_, params.key, sessionInfo, params.options, player_.get(), this);
            session->SetRumbleCallback(OnRumbleStatic, nullptr);
            if (!session->Connect()) {
                FailStream("could not start the session");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                session_ = session;
                sessionGeneration_ = params.generation;
            }
            PostEvent("streamStarting", "{}");
        });
    }

    void OnStreamFailed(const Host &, IHS_StreamingResult result) override {
        FailStream(StreamingResultName(result), result);
    }

    // Session::Listener (ihslib session threads)

    void OnSessionConnected() override {
        streaming_ = true;
        PostStats(Stats());
        PostEvent("streamConnected", "{}");
    }

    void OnSessionDisconnected() override {
        streaming_ = false;
        auto session = CurrentSession();
        bool timedOut = session && session->Stats().timedOut;
        PostEvent("streamDisconnected",
                  std::string("{\"timedOut\":") + (timedOut ? "true" : "false") + "}");
    }

    void OnHostTitle(const std::string &title) override {
        PostEvent("hostTitle", "{\"title\":" + Quote(title) + "}");
    }

    void OnHostActivity(IHS_StreamActivity activity) override {
        PostEvent("hostActivity",
                  "{\"activity\":" + std::to_string(static_cast<int>(activity)) + "}");
    }

    void OnVideoFailed() override {
        bool restart = player_ && player_->restartNeeded();
        PostEvent("videoFailed",
                  std::string("{\"restartNeeded\":") + (restart ? "true" : "false") + "}");
        worker_.Post([this] {
            if (auto session = CurrentSession()) session->Disconnect();
        });
    }

    void OnSessionFinalized() override {
        streaming_ = false;
        worker_.Post([this] {
            std::shared_ptr<Session> session;
            uint64_t finishedGen = 0;
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                session = std::move(session_);
                finishedGen = sessionGeneration_;
            }
            if (session) {
                session->Join();
                while (session.use_count() > 1) usleep(1000);
                session.reset();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pendingStream_ && pendingStream_->generation == finishedGen) {
                    pendingStream_.reset();
                    streamRequested_ = false;
                }
            }
            PostStats(Stats());
            PostEvent("streamEnded", "{}");
        });
    }

    std::shared_ptr<Session> CurrentSession() {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        return session_;
    }

    void FailStream(const std::string &reason, int result = -1) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingStream_.reset();
            streamRequested_ = false;
        }
        PostEvent("streamFailed",
                  "{\"result\":" + std::to_string(result) + ",\"reason\":" + Quote(reason) + "}");
    }

    // Gamepad handling (supports both PPB_Gamepad and browser JS navigator.getGamepads)

    void HandleJsGamepadSample(const pp::VarDictionary &dict) {
        int slot = dict.Get("slot").AsInt();
        if (slot < 0 || slot >= TSL_GAMEPAD_SLOTS) return;
        std::lock_guard<std::mutex> lock(js_pad_mutex_);
        RawPadSample &raw = js_pads_[slot];
        raw = RawPadSample{};
        raw.connected = dict.Get("connected").AsBool();
        if (!raw.connected) return;
        std::string id = dict.Get("id").AsString();
        std::snprintf(raw.id, sizeof(raw.id), "%s", id.empty() ? "Gamepad" : id.c_str());

        pp::Var axes_var = dict.Get("axes");
        if (axes_var.is_array()) {
            pp::VarArray axes_arr(axes_var);
            size_t n = axes_arr.GetLength();
            if (n > 16) n = 16;
            raw.numAxes = n;
            for (size_t i = 0; i < n; ++i) {
                raw.axes[i] = axes_arr.Get(static_cast<uint32_t>(i)).AsDouble();
            }
        }
        pp::Var btns_var = dict.Get("buttons");
        if (btns_var.is_array()) {
            pp::VarArray btns_arr(btns_var);
            size_t n = btns_arr.GetLength();
            if (n > 64) n = 64;
            raw.numButtons = n;
            for (size_t i = 0; i < n; ++i) {
                double val = btns_arr.Get(static_cast<uint32_t>(i)).AsDouble();
                raw.buttons[i] = val;
                raw.pressed[i] = val >= 0.5;
            }
        }
    }

    uint32_t SampleGamepads(RawPadSample (&samples)[TSL_GAMEPAD_SLOTS]) {
        for (int i = 0; i < TSL_GAMEPAD_SLOTS; ++i) {
            samples[i] = RawPadSample{};
        }
        uint32_t nativeMask = 0;
        if (gamepad_api_) {
            PP_GamepadsSampleData data{};
            gamepad_api_->Sample(pp_instance(), &data);
            uint32_t count = data.length;
            if (count > TSL_GAMEPAD_SLOTS) count = TSL_GAMEPAD_SLOTS;
            for (uint32_t i = 0; i < count; ++i) {
                const PP_GamepadSampleData &item = data.items[i];
                if (!item.connected) continue;
                nativeMask |= (1u << i);
                RawPadSample &raw = samples[i];
                raw.connected = true;
                size_t pos = 0;
                for (size_t c = 0; c < 128 && item.id[c] != 0 && pos + 1 < sizeof(raw.id); ++c) {
                    uint16_t ch = item.id[c];
                    raw.id[pos++] = (ch < 0x80) ? static_cast<char>(ch) : '?';
                }
                raw.id[pos] = '\0';
                if (pos == 0) std::snprintf(raw.id, sizeof(raw.id), "Gamepad");

                size_t axes_n = item.axes_length;
                if (axes_n > 16) axes_n = 16;
                raw.numAxes = axes_n;
                for (size_t a = 0; a < axes_n; ++a) {
                    raw.axes[a] = item.axes[a];
                }
                size_t btn_n = item.buttons_length;
                if (btn_n > 64) btn_n = 64;
                raw.numButtons = btn_n;
                for (size_t b = 0; b < btn_n; ++b) {
                    raw.buttons[b] = item.buttons[b];
                    raw.pressed[b] = item.buttons[b] >= 0.5f;
                }
            }
        }
        std::lock_guard<std::mutex> lock(js_pad_mutex_);
        for (int i = 0; i < TSL_GAMEPAD_SLOTS; ++i) {
            if (!samples[i].connected && js_pads_[i].connected) {
                samples[i] = js_pads_[i];
            }
        }
        return nativeMask;
    }

    void PollGamepads() {
        using Clock = std::chrono::steady_clock;
        constexpr auto kStatsPeriod = std::chrono::milliseconds(1000);
        const uint16_t exitCombo = (1u << TSL_PAD_LEFT_SHOULDER) | (1u << TSL_PAD_RIGHT_SHOULDER) |
                                   (1u << TSL_PAD_BACK) | (1u << TSL_PAD_START);
        bool exitSent = false;
        bool padConnected[TSL_GAMEPAD_SLOTS] = {};
        bool padHeld[TSL_GAMEPAD_SLOTS] = {};
        uint32_t lastNativeMask = 0xFFFFFFFFu;
        const Session *lastSession = nullptr;
        Clock::time_point lastStats = Clock::now();

        while (polling_) {
            std::shared_ptr<Session> session = streaming_ ? CurrentSession() : nullptr;
            if (!session) {
                exitSent = false;
                usleep(50 * 1000);
                continue;
            }

            Clock::time_point now = Clock::now();
            if (now - lastStats >= kStatsPeriod) {
                lastStats = now;
                PostStats(Stats());
            }

            if (session.get() != lastSession) {
                lastSession = session.get();
                lastNativeMask = 0xFFFFFFFFu;
                for (bool &c : padConnected) c = false;
            }

            RawPadSample samples[TSL_GAMEPAD_SLOTS];
            uint32_t nativeMask = SampleGamepads(samples);
            if (nativeMask != lastNativeMask) {
                lastNativeMask = nativeMask;
                ppbGamepadMask_.store(nativeMask, std::memory_order_relaxed);
                PostEvent("ppbPads", "{\"mask\":" + std::to_string(nativeMask) + "}");
            }

            for (int i = 0; i < TSL_GAMEPAD_SLOTS; i++) {
                const RawPadSample &pad = samples[i];
                bool connected = pad.connected;
                if (connected != padConnected[i]) {
                    TSL_GamepadInfo info{};
                    if (connected) {
                        if (!TSL_GamepadParseIds(pad.id, &info.vendorId, &info.productId)) {
                            TSL_GamepadGuessIds(pad.id, &info.vendorId, &info.productId);
                        }
                        std::snprintf(info.name, sizeof(info.name), "%s", pad.id);
                    }
                    session->SetGamepadConnected(i, connected, &info);
                    padConnected[i] = connected;
                    PostEvent(connected ? "gamepadConnected" : "gamepadDisconnected",
                              "{\"slot\":" + std::to_string(i) + ",\"id\":" +
                                      Quote(connected ? pad.id : "") + "}");
                }
                if (!connected) continue;

                TSL_GamepadState state;
                TSL_GamepadStateFromStandard(pad.axes, pad.numAxes, pad.buttons, pad.pressed,
                                             pad.numButtons, &state);
                if (!padInput_) {
                    padHeld[i] = true;
                    session->UpdateGamepad(i, TSL_GamepadState{});
                    continue;
                }
                if (padHeld[i]) {
                    const int16_t triggerUp = 4096;
                    if (state.buttons != 0 || state.axes[TSL_PAD_AXIS_TRIGGER_LEFT] > triggerUp ||
                        state.axes[TSL_PAD_AXIS_TRIGGER_RIGHT] > triggerUp) {
                        session->UpdateGamepad(i, TSL_GamepadState{});
                        continue;
                    }
                    padHeld[i] = false;
                    exitSent = false;
                }
                session->UpdateGamepad(i, state);
                if ((state.buttons & exitCombo) == exitCombo && !exitSent) {
                    exitSent = true;
                    PostEvent("exitCombo", "{}");
                }
            }
            session.reset();
            usleep(4 * 1000);
        }
    }

    const PPB_Gamepad *gamepad_api_ = nullptr;
    pp::CompletionCallbackFactory<TslInstance> callback_factory_;

    std::mutex outbox_mutex_;
    std::deque<std::pair<std::string, std::string>> outbox_;
    bool outbox_scheduled_ = false;

    Identity identity_;
    std::unique_ptr<Client> client_;
    std::unique_ptr<Player> player_;
    Worker worker_;

    std::mutex mutex_;
    std::map<std::string, Host> hosts_;
    std::string pendingPair_, pendingPin_;
    bool pairActive_ = false;
    std::optional<StreamParams> pendingStream_;
    bool streamRequested_ = false;
    uint64_t streamGeneration_ = 0;

    std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;
    uint64_t sessionGeneration_ = 0;

    std::mutex js_pad_mutex_;
    RawPadSample js_pads_[TSL_GAMEPAD_SLOTS]{};

    std::atomic<bool> polling_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> padInput_{true};
    std::atomic<uint32_t> ppbGamepadMask_{0};
    std::thread gamepadThread_;
};

std::mutex TslInstance::s_instanceMutex_;
TslInstance *TslInstance::s_instance_ = nullptr;

class TslModule : public pp::Module {
public:
    TslModule() : pp::Module() {}
    ~TslModule() override {}

    pp::Instance *CreateInstance(PP_Instance instance) override {
        return new TslInstance(instance);
    }
};

}  // namespace
}  // namespace tsl

namespace pp {
Module *CreateModule() {
    return new tsl::TslModule();
}
}  // namespace pp
