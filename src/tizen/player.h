#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <mutex>
#include <vector>

#include <opus.h>

#include "core/counter.h"
#include "core/session.h"
#include "core/counter.h"

#include "samsung/html/html_media_element.h"
#include "samsung/wasm/elementary_media_stream_source.h"
#include "samsung/wasm/elementary_media_stream_source_listener.h"
#include "samsung/wasm/elementary_media_track.h"
#include "samsung/wasm/elementary_media_track_listener.h"
#include "samsung/wasm/media_pipeline_error.h"
#include "samsung/wasm/operation_result.h"

namespace tsl {

// Hardware playback through Samsung's ElementaryMediaStreamSource (Tizen WASM Player) on a
// <video> element. Video goes in as Annex-B access units; audio is decoded from Opus to
// PCM here, as Moonlight for Tizen does.
//
// The source is opened once per page load, on the first VideoStart. Most TVs reload the page
// after a stream ends. The legacy player of 2020 TVs (Tizen 5.5) can't take a reload after a
// stream (it hung the browser on a Q74T), so there the page stays and later streams reuse the
// tracks, which then fix the codec, HDR profile and audio until the app starts again.
class Player : public MediaSink {
public:
    struct Counters {
        uint64_t videoAppended = 0;
        uint64_t videoFailed = 0;
        uint64_t audioAppended = 0;
        uint64_t audioFailed = 0;
        uint64_t silenceFilled = 0;
        uint64_t audioDropped = 0;
        uint64_t audioConcealed = 0;
        // Diagnostics: what the platform player reported.
        uint64_t videoSessionChanges = 0;
        uint64_t videoTrackCloses = 0;
        int lastCloseReason = -1;
        int lastAppendError = 0;
        uint64_t pipelineErrors = 0;
        int lastPipelineError = -1;
        // Diagnostics: audio timestamp resets (audible gaps), audio lead over the clock, and
        // the longest gap between video frames and the slowest AppendPacket since the last read.
        uint64_t audioGaps = 0;
        int audioLeadMs = 0;
        int maxFrameGapMs = 0;
        int maxAppendMs = 0;
    };

    // Must be constructed on the browser main thread. videoElementId names a <video>.
    // latency is fixed for the page's lifetime: Samsung's player takes it at construction.
    Player(const char *videoElementId, samsung::wasm::EmssLatencyMode latency);
    ~Player() override;

    void SetHevcSupported(bool supported) { hevc_ = supported; }
    // Before the first VideoStart: HDR was asked for, so HEVC gets the 10-bit profile.
    void SetHdr(bool hdr) { hdr_ = hdr; }
    void SetAudioEnabled(bool enabled) { audioEnabled_ = enabled; }
    // Before the first VideoStart: the video track takes it when it opens.
    void SetFramerate(uint32_t fps) { fps_ = fps > 0 ? fps : 60; }

    bool SupportsHevc() override { return hevc_; }
    int VideoStart(const IHS_StreamVideoConfig &config) override;
    IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *data, size_t len, bool keyframe) override;
    void VideoStop() override;
    int AudioStart(const IHS_StreamAudioConfig &config) override;
    int AudioPacket(const uint8_t *data, size_t len) override;
    void AudioLost(uint32_t frames) override;
    void AudioStop() override;

    Counters counters();
    // A stream needed other tracks than the ones the page set up, or setting them up failed:
    // only a new page (app start) can play it.
    bool restartNeeded() const { return restartNeeded_; }
    // VideoStart has set the tracks up for this page.
    bool setUp() const { return sourceUsed_; }


    using Seconds = samsung::wasm::Seconds;

    // How far audio timestamps may drift from the wall clock. The default is 40 ms both ways:
    // in the low latency modes the player's clock follows the timestamps appended, so audio
    // running far ahead made video frames count as late. With 150 ms ahead allowed, an HEVC
    // stream on a Samsung monitor (Tizen 9) dropped about 100 frames at the start, with 40 ms
    // about 20.
    struct AudioTiming {
        Seconds maxBehind{0.04};   // further behind: jump forward (a gap)
        Seconds resetLead{0};      // after a jump, start this far ahead
        Seconds drainAbove{0.04};  // further ahead: drop one packet in drainEvery
        unsigned drainEvery = 1;
        Seconds maxAhead{0.04};    // further ahead: drop every packet
    };
    // For the legacy player of 2020 TVs (Tizen 5.5). With the default, a Q74T threw away about
    // 1.6 s of sound a minute from a Steam Deck on Wi-Fi, in 18 gaps: each network hiccup made
    // a gap and then dropped the burst that followed. With this, 0.6 s and 4 gaps, for about
    // 30 ms more audio lead on average, and still no video frame dropped.
    static AudioTiming WideAudioTiming() {
        AudioTiming t;
        t.maxBehind = Seconds(0.05);
        t.resetLead = Seconds(0.04);
        t.drainAbove = Seconds(0.08);
        t.drainEvery = 8;
        t.maxAhead = Seconds(0.2);
        return t;
    }
    // Before the first stream.
    void SetAudioTiming(const AudioTiming &timing) { audioTiming_ = timing; }

private:
    using ReadyState = samsung::wasm::ElementaryMediaStreamSource::ReadyState;

    class SourceListener : public samsung::wasm::ElementaryMediaStreamSourceListener {
    public:
        explicit SourceListener(Player *player) : player_(player) {}
        void OnSourceClosed() override;
        void OnSourceOpenPending() override;
        void OnSourceOpen() override;
        void OnPipelineError(samsung::wasm::MediaPipelineError error, const char *message) override;

    private:
        Player *player_;
    };

    class TrackListener : public samsung::wasm::ElementaryMediaTrackListener {
    public:
        TrackListener(Player *player, bool video) : player_(player), video_(video) {}
        void OnTrackOpen() override;
        void OnTrackClosed(samsung::wasm::ElementaryMediaTrack::CloseReason reason) override;
        void OnSessionIdChanged(samsung::wasm::SessionId sessionId) override;
        void OnAppendError(samsung::wasm::OperationResult result) override;

    private:
        Player *player_;
        bool video_;
    };

    bool OpenSource(const IHS_StreamVideoConfig &config);
    bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()> &condition);
    Seconds Now() const;
    void AppendPcm(const int16_t *pcm, int samplesPerChannel);
    void AppendDecoded(int samplesPerChannel);  // pcm_, audioMutex_ held
    void FillSilenceIfStarved();

    std::atomic<bool> hevc_{false};
    std::atomic<bool> hdr_{false};
    std::atomic<bool> audioEnabled_{true};

    std::mutex mutex_;
    std::condition_variable changed_;
    ReadyState sourceState_ = ReadyState::kDetached;
    // Set up on the first VideoStart and kept for the page: see VideoStart.
    std::atomic<bool> sourceUsed_{false};
    bool sourceOpened_ = false;
    int openCodec_ = -1;
    bool openAudio_ = false;
    bool openMain10_ = false;
    std::atomic<bool> restartNeeded_{false};
    std::atomic<bool> videoOpen_{false};
    std::atomic<bool> audioOpen_{false};
    std::atomic<samsung::wasm::SessionId> videoSession_{0};
    std::atomic<samsung::wasm::SessionId> audioSession_{0};

    samsung::html::HTMLMediaElement element_;
    samsung::wasm::ElementaryMediaStreamSource source_;
    SourceListener sourceListener_;
    TrackListener videoListener_;
    TrackListener audioListener_;
    samsung::wasm::ElementaryMediaTrack videoTrack_;
    samsung::wasm::ElementaryMediaTrack audioTrack_;

    std::chrono::steady_clock::time_point start_;
    uint32_t width_ = 0, height_ = 0, fps_ = 60;
    uint32_t trackFps_ = 60;  // what the video track was opened with: fps_, or less if the TV refused it
    bool waitKeyframe_ = true;
    bool lostReported_ = false;

    std::mutex audioMutex_;
    OpusDecoder *opus_ = nullptr;
    int channels_ = 2;
    std::vector<int16_t> pcm_;
    int lastSamples_ = 480;  // per frame, for concealment
    Seconds audioPts_{0};
    AudioTiming audioTiming_;
    unsigned drainCount_ = 0;
    std::chrono::steady_clock::time_point lastAudio_;

    Counter64 videoAppended_{0}, videoFailed_{0}, audioAppended_{0}, audioFailed_{0}, silence_{0},
            audioDropped_{0};
    Counter64 videoSessionChanges_{0}, videoTrackCloses_{0}, pipelineErrors_{0}, audioConcealed_{0};
    std::atomic<int> lastCloseReason_{-1}, lastAppendError_{0}, lastPipelineError_{-1};
    Counter64 audioGaps_{0};
    std::atomic<int> audioLeadUs_{0}, maxFrameGapUs_{0}, maxAppendUs_{0};
    std::chrono::steady_clock::time_point lastFrame_;
};

}  // namespace tsl
