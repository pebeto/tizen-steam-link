// The WASM side of the TV app. JavaScript calls the functions bound at the bottom of this
// file (on the browser main thread, which must never block); results come back as events
// through window.tslEvent(type, json).
//
// Threads: ihslib runs its own client and session threads. Anything that can block
// (creating, joining, destroying a session) runs on `worker_`. Gamepads are polled on
// their own thread while a stream is up.
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/html5.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/client.h"
#include "core/gamepad.h"
#include "core/identity.h"
#include "core/keymap.h"
#include "core/log.h"
#include "core/session.h"
#include "samsung/wasm/emss_version_info.h"
#include "tizen/player.h"

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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string Quote(const std::string &s) { return "\"" + JsonEscape(s) + "\""; }

// Hands an event to JavaScript on the main thread without waiting for it. The strings are
// copied to the heap because the call may run after this function returned.
void PostEvent(const std::string &type, const std::string &json) {
    char *t = strdup(type.c_str());
    char *j = strdup(json.c_str());
    MAIN_THREAD_ASYNC_EM_ASM(
            {
                var type = UTF8ToString($0);
                var data = UTF8ToString($1);
                _free($0);
                _free($1);
                if (typeof window.tslEvent === 'function') {
                    window.tslEvent(type, data);
                }
            },
            t, j);
}

void LogToConsole(IHS_LogLevel level, const char *tag, const char *message) {
    std::printf("[%s %s] %s\n", IHS_LogLevelName(level), tag, message);
    // Info and above also go to the UI's log overlay (blue key), the only log a TV user
    // can read without sdb.
    if (level <= IHS_LogLevelInfo) {
        PostEvent("log", "{\"level\":" + std::to_string(level) + ",\"tag\":" + Quote(tag) +
                                 ",\"message\":" + Quote(message) + "}");
    }
}

std::string HostJson(const Host &host) {
    return "{\"ip\":" + Quote(host.address()) + ",\"hostname\":" + Quote(host.hostname()) +
           ",\"ostype\":" + std::to_string(host.info.ostype) + ",\"universe\":" +
           std::to_string(host.info.universe) + ",\"gamesRunning\":" + (host.info.gamesRunning ? "true" : "false") +
           ",\"mac\":" + Quote(host.macAddress()) + "}";
}

// A single thread that runs posted jobs in order.
class Worker {
public:
    Worker() : thread_([this] { Run(); }) {}
    ~Worker() {
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
    std::string ip;
    uint64_t steamId = 0;
    HostKey key{};
    StreamRequest request;
    SessionOptions options;
};

class App : public Client::Listener, public Session::Listener {
public:
    explicit App(samsung::wasm::EmssLatencyMode latency) : player_("tsl-video", latency) {}

    Player &player() { return player_; }

    ~App() override {
        polling_ = false;
        if (gamepadThread_.joinable()) gamepadThread_.join();
    }

    bool Init(const std::string &serialized) {
        if (client_) return true;
        auto identity = Identity::Parse(serialized);
        if (!identity) return false;
        identity_ = *identity;
        client_ = std::make_unique<Client>(identity_, this);
        polling_ = true;
        gamepadThread_ = std::thread([this] { PollGamepads(); });
        return true;
    }

    // Discovery and probes send UDP packets, and Samsung's socket extension can't run on the
    // browser thread (Tizen 5.5): on the worker.
    void StartDiscovery() {
        if (client_) worker_.Post([this] { client_->StartDiscovery(2000); });
    }

    void StopDiscovery() {
        if (client_) worker_.Post([this] { client_->StopDiscovery(); });
    }

    // Reports whether the address is valid; the probe itself goes out on the worker.
    bool Probe(const std::string &ip) {
        IHS_IPAddress address{};
        if (!client_ || !IHS_IPAddressFromString(&address, ip.c_str())) return false;
        worker_.Post([this, ip] { client_->Probe(ip); });
        return true;
    }

    // Sockets block: off the main thread.
    bool Wake(const std::string &mac) {
        if (!client_ || !WakePacket(mac)) return false;
        worker_.Post([this, mac] { client_->Wake(mac); });
        return true;
    }

    bool Pair(const std::string &ip, const std::string &pin) {
        if (!client_) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pairActive_) return false;  // ihslib runs one pairing at a time
            pendingPair_ = ip;
            pendingPin_ = pin;
        }
        // The host answers the probe through OnHostFound, which starts the pairing.
        return Probe(ip);
    }

    // Only cancels a pairing ihslib is still running. Once it reported a result, its task and
    // state are being freed on the timer thread and IHS_ClientAuthorizationCancel would read
    // them. Runs on the worker, after any queued Pair job.
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

    bool StartStream(const StreamParams &params) {
        if (!client_) return false;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_) return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingStream_) return false;
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
        worker_.Post([this] {
            if (auto session = CurrentSession()) session->Disconnect();
        });
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

    // Off while the UI's stream menu is open: the pads drive the menu, the computer sees them
    // released.
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
        Player::Counters c = player_.counters();
        return "{\"connected\":" + std::string(active ? "true" : "false") +
               ",\"videoFrames\":" + std::to_string(s.videoFrames) + ",\"keyFrames\":" +
               std::to_string(s.keyFrames) + ",\"videoBytes\":" + std::to_string(s.videoBytes) +
               ",\"audioPackets\":" + std::to_string(s.audioPackets) + ",\"width\":" + std::to_string(s.width) +
               ",\"height\":" + std::to_string(s.height) + ",\"videoCodec\":" + std::to_string(s.videoCodec) +
               ",\"colorspace\":" + std::to_string(s.colorspace) +
               ",\"targetBitrate\":" + std::to_string(s.targetBitrate) + ",\"framerateNum\":" +
               std::to_string(s.framerateNum) + ",\"framerateDen\":" + std::to_string(s.framerateDen) +
               ",\"videoAppended\":" + std::to_string(c.videoAppended) + ",\"videoFailed\":" +
               std::to_string(c.videoFailed) + ",\"audioAppended\":" + std::to_string(c.audioAppended) +
               ",\"audioFailed\":" + std::to_string(c.audioFailed) + ",\"silenceFilled\":" +
               std::to_string(c.silenceFilled) + ",\"audioDropped\":" + std::to_string(c.audioDropped) +
               ",\"videoPackets\":" + std::to_string(s.videoPackets) + ",\"videoPacketsLost\":" +
               std::to_string(s.videoPacketsLost) + ",\"lostReports\":" + std::to_string(s.lostReports) +
               ",\"roundTripMs\":" + std::to_string(s.roundTripMs) + ",\"fec\":" + (s.fec ? "true" : "false") +
               ",\"fecRecovered\":" + std::to_string(s.fecRecovered) + ",\"fecUnrecoverable\":" +
               std::to_string(s.fecUnrecoverable) + ",\"videoSessionChanges\":" +
               std::to_string(c.videoSessionChanges) + ",\"videoTrackCloses\":" + std::to_string(c.videoTrackCloses) +
               ",\"lastCloseReason\":" + std::to_string(c.lastCloseReason) + ",\"lastAppendError\":" +
               std::to_string(c.lastAppendError) + ",\"pipelineErrors\":" + std::to_string(c.pipelineErrors) +
               ",\"lastPipelineError\":" + std::to_string(c.lastPipelineError) + ",\"audioConcealed\":" +
               std::to_string(c.audioConcealed) + ",\"audioGaps\":" + std::to_string(c.audioGaps) +
               ",\"audioLeadMs\":" + std::to_string(c.audioLeadMs) + ",\"maxFrameGapMs\":" +
               std::to_string(c.maxFrameGapMs) + ",\"maxAppendMs\":" + std::to_string(c.maxAppendMs) + "}";
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
        // The UI stores the key with the host and hands it back to startStream.
        PostEvent("pairSucceeded", "{\"ip\":" + Quote(host.address()) + ",\"hostname\":" + Quote(host.hostname()) +
                                           ",\"steamId\":\"" + std::to_string(steamId) + "\",\"key\":\"" +
                                           HexEncode(key.data(), key.size()) + "\"}");
    }

    void OnPairFailed(const Host &host, IHS_AuthorizationResult result) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pairActive_ = false;
        }
        PostEvent("pairFailed", "{\"ip\":" + Quote(host.address()) + ",\"result\":" + std::to_string(result) +
                                        ",\"reason\":" + Quote(AuthorizationResultName(result)) + "}");
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
                // Canceled meanwhile, or a late answer to an earlier request.
                if (!pendingStream_ || pendingStream_->ip != ip) return;
                params = *pendingStream_;
            }
            IHS_SessionInfo sessionInfo = info;
            sessionInfo.steamId = params.steamId;
            player_.SetHevcSupported(params.options.hevc);
            player_.SetHdr(params.options.hdr);
            player_.SetAudioEnabled(params.options.audio);
            player_.SetFramerate(static_cast<uint32_t>(params.options.maxFramerate));
            auto session =
                    std::make_shared<Session>(identity_, params.key, sessionInfo, params.options, &player_, this);
            session->SetRumbleCallback(OnRumble, nullptr);
            if (!session->Connect()) {
                FailStream("could not start the session");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                session_ = session;
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
        PostEvent("streamConnected", "{}");
    }

    void OnSessionDisconnected() override {
        streaming_ = false;
        auto session = CurrentSession();
        bool timedOut = session && session->Stats().timedOut;
        PostEvent("streamDisconnected", std::string("{\"timedOut\":") + (timedOut ? "true" : "false") + "}");
    }

    void OnVideoFailed() override {
        PostEvent("videoFailed", std::string("{\"restartNeeded\":") + (player_.restartNeeded() ? "true" : "false") + "}");
        worker_.Post([this] {
            if (auto session = CurrentSession()) session->Disconnect();
        });
    }

    void OnSessionFinalized() override {
        streaming_ = false;
        // Runs on the session thread itself, which Join() waits for: hand off.
        worker_.Post([this] {
            std::shared_ptr<Session> session;
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                session = std::move(session_);
            }
            if (session) {
                session->Join();
                // Input calls may still hold a copy for a moment. Destroy it here, not on
                // whichever thread happens to drop the last reference.
                while (session.use_count() > 1) usleep(1000);
                session.reset();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingStream_.reset();
                streamRequested_ = false;
            }
            PostEvent("streamEnded", "{}");
        });
    }

private:
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
        PostEvent("streamFailed", "{\"result\":" + std::to_string(result) + ",\"reason\":" + Quote(reason) + "}");
    }

    static void OnRumble(int slot, uint16_t low, uint16_t high, uint32_t durationMs, void *) {
        PostEvent("rumble", "{\"slot\":" + std::to_string(slot) + ",\"low\":" + std::to_string(low) +
                                    ",\"high\":" + std::to_string(high) + ",\"ms\":" + std::to_string(durationMs) +
                                    "}");
    }

    void PollGamepads() {
        const uint16_t exitCombo = (1u << TSL_PAD_LEFT_SHOULDER) | (1u << TSL_PAD_RIGHT_SHOULDER) |
                                   (1u << TSL_PAD_BACK) | (1u << TSL_PAD_START);
        bool exitSent = false;
        bool padConnected[TSL_GAMEPAD_SLOTS] = {};
        // After the menu, a pad stays released for the computer until every button and trigger
        // is let go: the press that closed the menu isn't the computer's.
        bool padHeld[TSL_GAMEPAD_SLOTS] = {};
        const Session *lastSession = nullptr;
        while (polling_) {
            std::shared_ptr<Session> session = streaming_ ? CurrentSession() : nullptr;
            if (!session) {
                exitSent = false;
                usleep(50 * 1000);
                continue;
            }
            if (session.get() != lastSession) {
                // A new session knows no pads yet; announce every connected one again.
                lastSession = session.get();
                for (bool &c : padConnected) c = false;
            }
            if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
                usleep(200 * 1000);
                continue;
            }
            int count = emscripten_get_num_gamepads();
            for (int i = 0; i < TSL_GAMEPAD_SLOTS; i++) {
                EmscriptenGamepadEvent pad;
                bool connected = i < count && emscripten_get_gamepad_status(i, &pad) == EMSCRIPTEN_RESULT_SUCCESS &&
                                 pad.connected;
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
                              "{\"slot\":" + std::to_string(i) + ",\"id\":" + Quote(connected ? pad.id : "") + "}");
                }
                if (!connected) continue;
                bool pressed[64];
                int buttons = pad.numButtons < 64 ? pad.numButtons : 64;
                for (int b = 0; b < buttons; b++) pressed[b] = pad.digitalButton[b] != 0;
                TSL_GamepadState state;
                TSL_GamepadStateFromStandard(pad.axis, static_cast<size_t>(pad.numAxes), pad.analogButton, pressed,
                                             static_cast<size_t>(buttons), &state);
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

    Identity identity_;
    std::unique_ptr<Client> client_;
    Player player_;
    Worker worker_;

    std::mutex mutex_;
    std::map<std::string, Host> hosts_;
    std::string pendingPair_, pendingPin_;
    bool pairActive_ = false;
    std::optional<StreamParams> pendingStream_;
    bool streamRequested_ = false;

    // Guards the pointer only. Callers copy it and call the session without the lock, so
    // the main thread never waits behind a slow ihslib call on another thread.
    std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;

    std::atomic<bool> polling_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> padInput_{true};
    std::thread gamepadThread_;
};

App *g_app = nullptr;
int g_tizenVersion = 0;
bool g_ultraLowAvailable = false;
// The legacy WASM player of 2020 TVs (Tizen 5.5): the page keeps its player across streams
// instead of reloading, and streams go on after a lost frame by default (see app.js).
bool g_legacyPlayer = false;
samsung::wasm::EmssLatencyMode g_latency = samsung::wasm::EmssLatencyMode::kLow;

// The player's latency mode is fixed when it is created, before the stream's codec is known
// (the page reloads when a setting change changes the mode).
//
// Up to Tizen 8, ultra low latency is the lowest latency and switches the TV to Game Mode.
// Tizen 9 changed Game Mode for WebAssembly apps: ultra low latency no longer switches it on,
// and on a Samsung M7 smart monitor (Tizen 9) it decoded HEVC without showing a frame past
// the first, and dropped about 17% of H.264 frames. So Tizen 9 and later get low latency,
// and Game Mode comes from the use.game.mode metadata in config.xml instead, as in Moonlight
// for Tizen. HEVC gets low latency on every version, since nothing older was tried with it.
//
// The latency setting overrides that rule: 1 is low, 2 ultra low (where the TV has it). On a
// QN90D TV (Tizen 9) H.264 plays in ultra low latency, but HEVC, with or without HDR, freezes
// on its first frame as on the M7, so HEVC always gets low latency. A Q74T (Tizen 5.5) has no
// ultra low latency at all.
samsung::wasm::EmssLatencyMode LatencyFor(bool hevc, int latency) {
    bool ultraLow = latency == 0 ? g_tizenVersion < 9 && !hevc && g_ultraLowAvailable
                                 : latency == 2 && !hevc && g_ultraLowAvailable;
    return ultraLow ? samsung::wasm::EmssLatencyMode::kUltraLow : samsung::wasm::EmssLatencyMode::kLow;
}

// Bound functions. All run on the browser main thread.

std::string CreateIdentity(const std::string &randomHex, const std::string &name) {
    uint8_t bytes[Identity::kRandomBytes];
    if (!HexDecode(randomHex, bytes, sizeof(bytes)) || name.empty()) return "";
    return Identity::FromRandom(bytes, name).Serialize();
}

bool Init(const std::string &identity) { return g_app->Init(identity); }
void StartDiscovery() { g_app->StartDiscovery(); }
void StopDiscovery() { g_app->StopDiscovery(); }
bool Probe(const std::string &ip) { return g_app->Probe(ip); }
bool Wake(const std::string &mac) { return g_app->Wake(mac); }
bool Pair(const std::string &ip, const std::string &pin) { return g_app->Pair(ip, pin); }
void CancelPair() { g_app->CancelPair(); }

// quality: Steam's preference (-1 automatic, 1 fast, 2 balanced, 3 beautiful). bitrateKbps: -1 lets
// Steam decide. fps: 60 or 120. hdr: offer HDR10, which comes as HEVC, so it turns hevc on too.
// keepPlaying: after a lost video frame, go on decoding (a moment of smeared picture) instead of
// dropping every frame until a complete keyframe. From a Steam Deck on Wi-Fi to a Q74T, a 1080p
// keyframe at 30 Mbit/s often lost a packet too, and waiting froze the picture for 10 to 50 s.
// The UI turns it on by default only for the legacy player.
bool StartStream(const std::string &ip, const std::string &steamId, const std::string &keyHex, int maxWidth,
                 int maxHeight, bool hevc, bool audio, bool bigPicture, int quality, int bitrateKbps, int fps,
                 bool hdr, bool keepPlaying) {
    IHS_VideoWaitForKeyframe = !keepPlaying;
    StreamParams params;
    params.ip = ip;
    params.steamId = std::strtoull(steamId.c_str(), nullptr, 10);
    if (!HexDecode(keyHex, params.key.data(), params.key.size())) return false;
    params.request.maxWidth = maxWidth;
    params.request.maxHeight = maxHeight;
    params.request.interface = bigPicture ? IHS_StreamInterfaceBigPicture : IHS_StreamInterfaceDefault;
    params.request.hdr = hdr;
    params.options.hevc = hevc || hdr;
    params.options.hdr = hdr;
    params.options.audio = audio;
    params.options.quality = quality;
    params.options.bitrateKbps = bitrateKbps;
    params.options.maxWidth = maxWidth;
    params.options.maxHeight = maxHeight;
    params.options.maxFramerate = fps > 60 ? fps : 0;
    return g_app->StartStream(params);
}

void StopStream() { g_app->StopStream(); }
// Whether these codec and latency settings need another player latency mode, which only a page
// reload can give.
bool LatencyNeedsReload(bool hevc, int latency) { return LatencyFor(hevc, latency) != g_latency; }
bool LegacyPlayer() { return g_legacyPlayer; }
// The player's tracks are set up (the legacy player keeps them for the page).
bool PlayerSetUp() { return g_app->player().setUp(); }
void Key(const std::string &code, bool down) { g_app->Key(code, down); }
void SendText(const std::string &text) { g_app->SendText(text); }
void SetPadInput(bool enabled) { g_app->SetPadInput(enabled); }
void MouseMove(int dx, int dy) { g_app->MouseMove(dx, dy); }
void MouseButton(int button, bool down) { g_app->MouseButton(button, down); }
void MouseWheel(int deltaY) { g_app->MouseWheel(deltaY); }
std::string Stats() { return g_app->Stats(); }
void SetLogLevel(int level) { tsl::SetLogLevel(static_cast<IHS_LogLevel>(level)); }

}  // namespace
}  // namespace tsl

int main() {
    tsl::SetLogSink(tsl::LogToConsole);
    tsl::SetLogLevel(IHS_LogLevelInfo);
    IHS_Init();
    // The codec and latency settings pick the player's latency mode: see LatencyFor.
    bool hevc = EM_ASM_INT({
        try {
            return JSON.parse(localStorage.getItem('tsl.settings') || '{}').hevc ? 1 : 0;
        } catch (e) {
            return 0;
        }
    });
    int latency = EM_ASM_INT({
        try {
            return JSON.parse(localStorage.getItem('tsl.settings') || '{}').latency | 0;
        } catch (e) {
            return 0;
        }
    });
    // The major platform version, 9 for "9.0"; 0 outside a TV.
    int tizenVersion = EM_ASM_INT({
        try {
            return parseInt(tizen.systeminfo.getCapability('http://tizen.org/feature/platform.version'), 10) || 0;
        } catch (e) {
            return 0;
        }
    });
    // Older TVs may lack the ultra low latency mode altogether.
    auto emss = samsung::wasm::EmssVersionInfo::Create();
    tsl::Log(IHS_LogLevelInfo, "Player",
             "EMSS: emss=%d legacy=%d texture=%d decoding_mode=%d low_latency_texture=%d ultra_low=%d validation=%d",
             emss.has_emss, emss.has_legacy_emss, emss.has_video_texture, emss.has_decoding_mode,
             emss.has_low_latency_video_texture, emss.has_ultra_low_latency, emss.has_config_validation);
    tsl::g_tizenVersion = tizenVersion;
    tsl::g_ultraLowAvailable = emss.has_ultra_low_latency;
    tsl::g_legacyPlayer = emss.has_legacy_emss;
    auto mode = tsl::LatencyFor(hevc, latency);
    tsl::g_latency = mode;
    tsl::Log(IHS_LogLevelInfo, "Player", "%s latency mode (Tizen %d, codec setting %s, latency setting %s)",
             mode == samsung::wasm::EmssLatencyMode::kUltraLow ? "Ultra low" : "Low", tizenVersion,
             hevc ? "HEVC" : "H.264", latency == 1 ? "low" : latency == 2 ? "ultra low" : "automatic");
    tsl::g_app = new tsl::App(mode);
    if (emss.has_legacy_emss) tsl::g_app->player().SetAudioTiming(tsl::Player::WideAudioTiming());
    EM_ASM({
        if (typeof window.tslReady === 'function') window.tslReady();
    });
    return 0;
}

EMSCRIPTEN_BINDINGS(tsl) {
    emscripten::function("createIdentity", &tsl::CreateIdentity);
    emscripten::function("init", &tsl::Init);
    emscripten::function("startDiscovery", &tsl::StartDiscovery);
    emscripten::function("stopDiscovery", &tsl::StopDiscovery);
    emscripten::function("probe", &tsl::Probe);
    emscripten::function("wake", &tsl::Wake);
    emscripten::function("pair", &tsl::Pair);
    emscripten::function("cancelPair", &tsl::CancelPair);
    emscripten::function("startStream", &tsl::StartStream);
    emscripten::function("stopStream", &tsl::StopStream);
    emscripten::function("latencyNeedsReload", &tsl::LatencyNeedsReload);
    emscripten::function("legacyPlayer", &tsl::LegacyPlayer);
    emscripten::function("playerSetUp", &tsl::PlayerSetUp);
    emscripten::function("key", &tsl::Key);
    emscripten::function("sendText", &tsl::SendText);
    emscripten::function("setPadInput", &tsl::SetPadInput);
    emscripten::function("mouseMove", &tsl::MouseMove);
    emscripten::function("mouseButton", &tsl::MouseButton);
    emscripten::function("mouseWheel", &tsl::MouseWheel);
    emscripten::function("stats", &tsl::Stats);
    emscripten::function("setLogLevel", &tsl::SetLogLevel);
}
