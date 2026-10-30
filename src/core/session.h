#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/client.h"
#include "core/counter.h"
#include "core/gamepad.h"
#include "core/identity.h"
#include "core/ihs.h"

namespace tsl {

// Receives decoded-ready media from the session. All methods run on ihslib threads.
class MediaSink {
public:
    virtual ~MediaSink() = default;
    virtual bool SupportsHevc() { return false; }
    // Return 0 to accept the stream.
    virtual int VideoStart(const IHS_StreamVideoConfig &config) = 0;
    // One Annex-B access unit. Keyframes carry SPS/PPS.
    virtual IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *data, size_t len, bool keyframe) = 0;
    virtual void VideoStop() = 0;
    // Return 0 to accept the stream. Steam sends Opus.
    virtual int AudioStart(const IHS_StreamAudioConfig &config) = 0;
    virtual int AudioPacket(const uint8_t *data, size_t len) = 0;
    // Frames that never arrived, where they belong (before the next AudioPacket). Opus fills
    // a gap when decoding without data.
    virtual void AudioLost(uint32_t frames) { (void) frames; }
    virtual void AudioStop() = 0;
};

struct SessionOptions {
    bool audio = true;
    bool hevc = false;
    // Forward error correction on video: the host adds a parity packet to every video message,
    // so one lost packet per message is rebuilt instead of costing a keyframe.
    bool fec = true;
    // Steam's quality preference: -1 automatic, 1 fast, 2 balanced, 3 beautiful; 0 = balanced.
    int quality = 0;
    // Bitrate to ask for in kbit/s; 0 = 30000, -1 = let Steam decide.
    int bitrateKbps = 0;
    // Largest picture to ask for; 0 leaves Steam at 1920x1080. Pass the streaming request's
    // maximum: that request alone doesn't lift the stream above 1080p.
    int maxWidth = 0;
    int maxHeight = 0;
    // Frames per second to ask for; 0 = 59.94.
    int maxFramerate = 0;
    // Offer HDR10. Steam sends it, as 10-bit HEVC, when the host's display is in HDR, so it only
    // takes effect together with hevc. Set StreamRequest::hdr too.
    bool hdr = false;
};

struct SessionStats {
    uint64_t videoFrames = 0;
    uint64_t keyFrames = 0;
    uint64_t videoBytes = 0;
    uint64_t audioPackets = 0;
    uint64_t audioBytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    IHS_StreamVideoCodec videoCodec = IHS_StreamVideoCodecNone;
    IHS_StreamVideoColorspace colorspace = IHS_StreamVideoColorspaceUnknown;
    IHS_StreamAudioCodec audioCodec = IHS_StreamAudioCodecNone;
    int32_t targetBitrate = 0;  // kbit/s, as Steam sends it
    uint32_t framerateNum = 0;
    uint32_t framerateDen = 0;
    // Network, from ihslib (IHS_SessionStreamStats).
    uint32_t videoPackets = 0;
    uint32_t videoPacketsLost = 0;
    uint32_t lostReports = 0;
    float roundTripMs = 0;
    bool fec = false;
    uint32_t fecRecovered = 0;
    uint32_t fecUnrecoverable = 0;
    // The session ended because the host went silent, not by request.
    bool timedOut = false;
};

// One streaming session. Create it from Client::Listener::OnStreamReady, Connect(), and
// Disconnect() + Join() before destroying it. Join() must not run on an ihslib thread.
class Session {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void OnSessionConnected() {}
        virtual void OnSessionDisconnected() {}
        // Last callback; the session threads are about to exit.
        virtual void OnSessionFinalized() {}
        // The player could not start the video (a format the TV refuses). Runs on an ihslib
        // thread: disconnect from elsewhere.
        virtual void OnVideoFailed() {}
        virtual void OnCursorShown(float x, float y) { (void) x, (void) y; }
        virtual void OnCursorHidden() {}
    };

    // identity, media and listener must outlive the session. key is the host's pairing key.
    Session(const Identity &identity, const HostKey &key, const IHS_SessionInfo &info, const SessionOptions &options,
            MediaSink *media, Listener *listener);
    ~Session();

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool Connect();
    void Disconnect();
    void Join();

    bool connected() const { return connected_.load(); }
    SessionStats Stats() const;

    // Input. Scancodes are USB HID keyboard usages (what SDL calls scancodes).
    void SendKey(uint32_t scancode, bool down);
    void SendText(const char *utf8);
    void SendMouseMove(int dx, int dy);
    void SendMousePosition(float x, float y);
    void SendMouseButton(IHS_StreamInputMouseButton button, bool down);
    void SendMouseWheel(IHS_StreamInputMouseWheelDirection direction);

    void SetGamepadConnected(int slot, bool connected, const TSL_GamepadInfo *info);
    void UpdateGamepad(int slot, const TSL_GamepadState &state);
    void SetRumbleCallback(TSL_GamepadRumbleFunction *function, void *context);

private:
    static void CbConnected(IHS_Session *session, void *context);
    static void CbDisconnected(IHS_Session *session, void *context);
    static void CbFinalized(IHS_Session *session, void *context);
    static void CbConfiguring(IHS_Session *session, IHS_SessionConfig *config, void *context);
    static int CbVideoStart(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context);
    static IHS_StreamVideoSubmitResult CbVideoSubmit(IHS_Session *session, IHS_Buffer *data,
                                                     IHS_StreamVideoFrameFlag flags, void *context);
    static void CbVideoStop(IHS_Session *session, void *context);
    static void CbTargetFramerate(IHS_Session *session, uint32_t num, uint32_t den, uint32_t reasons,
                                  void *context);
    static void CbTargetBitrate(IHS_Session *session, int32_t bitrate, void *context);
    static int CbAudioStart(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context);
    static int CbAudioSubmit(IHS_Session *session, IHS_Buffer *data, void *context);
    static void CbAudioLost(IHS_Session *session, uint32_t frames, void *context);
    static void CbAudioStop(IHS_Session *session, void *context);
    static bool CbSetCursor(IHS_Session *session, uint64_t cursorId, void *context);
    static void CbShowCursor(IHS_Session *session, float x, float y, void *context);
    static void CbHideCursor(IHS_Session *session, void *context);

    HostKey key_;
    IHS_ClientConfig config_;
    SessionOptions options_;
    MediaSink *media_;
    Listener *listener_;
    IHS_Session *session_;
    IHS_HIDProvider *gamepads_;
    bool started_ = false;
    bool joined_ = false;
    std::atomic<bool> connected_{false};

    Counter64 videoFrames_{0}, keyFrames_{0}, videoBytes_{0}, audioPackets_{0}, audioBytes_{0};
    std::atomic<uint32_t> width_{0}, height_{0};
    std::atomic<int> videoCodec_{IHS_StreamVideoCodecNone}, audioCodec_{IHS_StreamAudioCodecNone};
    std::atomic<int> colorspace_{IHS_StreamVideoColorspaceUnknown};
    std::atomic<int32_t> bitrate_{0};
    std::atomic<uint32_t> fpsNum_{0}, fpsDen_{0};
};

}  // namespace tsl
