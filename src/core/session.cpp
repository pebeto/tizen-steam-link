#include "core/session.h"

#include "core/log.h"

namespace tsl {

Session::Session(const Identity &identity, const HostKey &key, const IHS_SessionInfo &info,
                 const SessionOptions &options, MediaSink *media, Listener *listener)
    : key_(key), config_(identity.ClientConfig()), options_(options), media_(media), listener_(listener) {
    config_.secretKey = key_.data();
    session_ = IHS_SessionCreate(&config_, &info);
    IHS_SessionSetLogFunction(session_, IhsLogFunction());

    static const IHS_StreamSessionCallbacks sessionCallbacks = {
            .initialized = nullptr,
            .connecting = nullptr,
            .configuring = CbConfiguring,
            .connected = CbConnected,
            .disconnected = CbDisconnected,
            .finalized = CbFinalized,
    };
    static const IHS_StreamVideoCallbacks videoCallbacks = {
            .start = CbVideoStart,
            .submit = CbVideoSubmit,
            .stop = CbVideoStop,
            .setCaptureSize = nullptr,
            .setTargetFramerate = CbTargetFramerate,
            .setTargetBitrate = CbTargetBitrate,
            .setQualityOverride = nullptr,
            .setBitrateOverride = nullptr,
    };
    static const IHS_StreamAudioCallbacks audioCallbacks = {
            .start = CbAudioStart,
            .submit = CbAudioSubmit,
            .stop = CbAudioStop,
            .lost = CbAudioLost,
    };
    static const IHS_StreamInputCallbacks inputCallbacks = {
            .setCursor = CbSetCursor,
            .deleteCursor = nullptr,
            .cursorImage = nullptr,
            .showCursor = CbShowCursor,
            .hideCursor = CbHideCursor,
            .setCapsLock = nullptr,
            .setKeymap = nullptr,
    };
    IHS_SessionSetSessionCallbacks(session_, &sessionCallbacks, this);
    IHS_SessionSetVideoCallbacks(session_, &videoCallbacks, this);
    IHS_SessionSetAudioCallbacks(session_, &audioCallbacks, this);
    IHS_SessionSetInputCallbacks(session_, &inputCallbacks, this);

    gamepads_ = TSL_GamepadProviderCreate();
    IHS_SessionHIDAddProvider(session_, gamepads_);
}

Session::~Session() {
    if (started_ && !joined_) {
        Disconnect();
        Join();
    }
    IHS_SessionDestroy(session_);
    // The session's HID manager detached the provider while being destroyed.
    TSL_GamepadProviderDestroy(gamepads_);
}

bool Session::Connect() {
    started_ = IHS_SessionConnect(session_);
    return started_;
}

void Session::Disconnect() { IHS_SessionDisconnect(session_); }

void Session::Join() {
    if (!started_ || joined_) return;
    IHS_SessionThreadedJoin(session_);
    joined_ = true;
}

SessionStats Session::Stats() const {
    SessionStats s;
    s.videoFrames = videoFrames_.load();
    s.keyFrames = keyFrames_.load();
    s.videoBytes = videoBytes_.load();
    s.audioPackets = audioPackets_.load();
    s.audioBytes = audioBytes_.load();
    s.width = width_.load();
    s.height = height_.load();
    s.videoCodec = static_cast<IHS_StreamVideoCodec>(videoCodec_.load());
    s.colorspace = static_cast<IHS_StreamVideoColorspace>(colorspace_.load());
    s.audioCodec = static_cast<IHS_StreamAudioCodec>(audioCodec_.load());
    s.targetBitrate = bitrate_.load();
    IHS_SessionStreamStats net;
    IHS_SessionGetStreamStats(session_, &net);
    s.videoPackets = net.videoPackets;
    s.videoPacketsLost = net.videoPacketsLost;
    s.lostReports = net.lostReports;
    s.roundTripMs = net.roundTripMs;
    s.fec = net.fec;
    s.fecRecovered = net.fecRecovered;
    s.fecUnrecoverable = net.fecUnrecoverable;
    s.timedOut = net.timedOut;
    s.framerateNum = fpsNum_.load();
    s.framerateDen = fpsDen_.load();
    return s;
}

void Session::SendKey(uint32_t scancode, bool down) {
    if (!connected_) return;
    if (down) {
        IHS_SessionSendKeyDown(session_, scancode);
    } else {
        IHS_SessionSendKeyUp(session_, scancode);
    }
}

void Session::SendText(const char *utf8) {
    if (!connected_) return;
    IHS_SessionSendText(session_, utf8);
}

void Session::SendMouseMove(int dx, int dy) {
    if (!connected_) return;
    IHS_SessionSendMouseMovement(session_, dx, dy);
}

void Session::SendMousePosition(float x, float y) {
    if (!connected_) return;
    IHS_SessionSendMousePosition(session_, x, y);
}

void Session::SendMouseButton(IHS_StreamInputMouseButton button, bool down) {
    if (!connected_) return;
    if (down) {
        IHS_SessionSendMouseDown(session_, button);
    } else {
        IHS_SessionSendMouseUp(session_, button);
    }
}

void Session::SendMouseWheel(IHS_StreamInputMouseWheelDirection direction) {
    if (!connected_) return;
    IHS_SessionSendMouseWheel(session_, direction);
}

void Session::SetGamepadConnected(int slot, bool connected, const TSL_GamepadInfo *info) {
    bool changed = TSL_GamepadProviderSetConnected(gamepads_, slot, connected, info);
    // Before the session is up the device list goes out from CbConnected instead.
    if (changed && connected_) {
        IHS_SessionHIDNotifyDeviceChange(session_);
    }
}

void Session::UpdateGamepad(int slot, const TSL_GamepadState &state) {
    TSL_GamepadProviderUpdate(gamepads_, slot, &state);
}

void Session::SetRumbleCallback(TSL_GamepadRumbleFunction *function, void *context) {
    TSL_GamepadProviderSetRumbleCallback(gamepads_, function, context);
}

void Session::CbConfiguring(IHS_Session *, IHS_SessionConfig *config, void *context) {
    auto *self = static_cast<Session *>(context);
    config->enableAudio = self->options_.audio;
    config->enableHevc = self->options_.hevc && self->media_->SupportsHevc();
    config->enableFec = self->options_.fec;
    config->quality = self->options_.quality;
    config->bitrateKbps = self->options_.bitrateKbps;
    config->maxWidth = self->options_.maxWidth;
    config->maxHeight = self->options_.maxHeight;
    config->maxFramerate = self->options_.maxFramerate;
    config->enableHdr = self->options_.hdr && config->enableHevc;  // HDR10 comes as HEVC
    Log(IHS_LogLevelInfo, "Session",
        "Negotiating: audio=%d hevc=%d hdr=%d fec=%d quality=%d bitrate=%d max=%dx%d@%d", config->enableAudio,
        config->enableHevc, config->enableHdr, config->enableFec, config->quality, config->bitrateKbps,
        config->maxWidth, config->maxHeight, config->maxFramerate);
}

void Session::CbConnected(IHS_Session *session, void *context) {
    auto *self = static_cast<Session *>(context);
    self->connected_ = true;
    IHS_SessionHIDNotifyDeviceChange(session);
    self->listener_->OnSessionConnected();
}

void Session::CbDisconnected(IHS_Session *, void *context) {
    auto *self = static_cast<Session *>(context);
    self->connected_ = false;
    self->listener_->OnSessionDisconnected();
}

void Session::CbFinalized(IHS_Session *, void *context) {
    static_cast<Session *>(context)->listener_->OnSessionFinalized();
}

int Session::CbVideoStart(IHS_Session *, const IHS_StreamVideoConfig *config, void *context) {
    auto *self = static_cast<Session *>(context);
    self->width_ = config->width;
    self->height_ = config->height;
    self->videoCodec_ = config->codec;
    self->colorspace_ = config->colorspace;
    Log(IHS_LogLevelInfo, "Session", "Video start: codec=%d %ux%u colorspace=%d", config->codec, config->width,
        config->height, config->colorspace);
    int result = self->media_->VideoStart(*config);
    if (result != 0) self->listener_->OnVideoFailed();
    return result;
}

IHS_StreamVideoSubmitResult Session::CbVideoSubmit(IHS_Session *, IHS_Buffer *data, IHS_StreamVideoFrameFlag flags,
                                                   void *context) {
    auto *self = static_cast<Session *>(context);
    bool keyframe = (flags & IHS_StreamVideoFrameKeyFrame) != 0;
    self->videoFrames_++;
    if (keyframe) self->keyFrames_++;
    self->videoBytes_ += data->size;
    return self->media_->VideoFrame(IHS_BufferPointer(data), data->size, keyframe);
}

void Session::CbVideoStop(IHS_Session *, void *context) {
    Log(IHS_LogLevelInfo, "Session", "Video stop");
    static_cast<Session *>(context)->media_->VideoStop();
}

void Session::CbTargetFramerate(IHS_Session *, uint32_t num, uint32_t den, uint32_t reasons, void *context) {
    auto *self = static_cast<Session *>(context);
    self->fpsNum_ = num;
    self->fpsDen_ = den;
    Log(IHS_LogLevelDebug, "Session", "Target framerate %u/%u (reasons 0x%x)", num, den, reasons);
}

void Session::CbTargetBitrate(IHS_Session *, int32_t bitrate, void *context) {
    static_cast<Session *>(context)->bitrate_ = bitrate;
}

int Session::CbAudioStart(IHS_Session *, const IHS_StreamAudioConfig *config, void *context) {
    auto *self = static_cast<Session *>(context);
    self->audioCodec_ = config->codec;
    Log(IHS_LogLevelInfo, "Session", "Audio start: codec=%d channels=%u rate=%u", config->codec, config->channels,
        config->frequency);
    return self->media_->AudioStart(*config);
}

int Session::CbAudioSubmit(IHS_Session *, IHS_Buffer *data, void *context) {
    auto *self = static_cast<Session *>(context);
    self->audioPackets_++;
    self->audioBytes_ += data->size;
    return self->media_->AudioPacket(IHS_BufferPointer(data), data->size);
}

void Session::CbAudioLost(IHS_Session *, uint32_t frames, void *context) {
    static_cast<Session *>(context)->media_->AudioLost(frames);
}

void Session::CbAudioStop(IHS_Session *, void *context) {
    Log(IHS_LogLevelInfo, "Session", "Audio stop");
    static_cast<Session *>(context)->media_->AudioStop();
}

bool Session::CbSetCursor(IHS_Session *, uint64_t, void *) {
    // Same answer as ihslib's sample client: every cursor counts as known, so the host
    // doesn't send cursor images. Nothing here draws a local cursor.
    return true;
}

void Session::CbShowCursor(IHS_Session *, float x, float y, void *context) {
    static_cast<Session *>(context)->listener_->OnCursorShown(x, y);
}

void Session::CbHideCursor(IHS_Session *, void *context) {
    static_cast<Session *>(context)->listener_->OnCursorHidden();
}

}  // namespace tsl
