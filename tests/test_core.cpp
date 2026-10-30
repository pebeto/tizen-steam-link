// Unit tests for the shared core. No framework: each test is a function, CHECK records
// failures, main() runs everything and returns non-zero if anything failed.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/client.h"
#include "core/counter.h"
#include "core/gamepad.h"
#include "core/identity.h"
#include "core/keymap.h"
#include "core/log.h"
#include "core/session.h"

extern "C" {
#include "hid/device.h"
#include "hid/manager.h"
#include "hid/provider.h"
#include "ihs_buffer.h"
#include "ihs_enumeration.h"
#include "protobuf/discovery.pb-c.h"
#include "protobuf/pb_utils.h"
#include "protobuf/remoteplay.pb-c.h"
#include "session/channels/ch_data.h"
#include "session/channels/ch_data_audio.h"
#include "session/channels/ch_data_microphone.h"
#include "session/fec.h"
#include "session/retransmission.h"
#include "session/session_pri.h"
#include "session/window.h"
}

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

void TestIdentityRoundTrip() {
    uint8_t random[tsl::Identity::kRandomBytes];
    for (size_t i = 0; i < sizeof(random); i++) random[i] = static_cast<uint8_t>(i * 7 + 1);
    tsl::Identity id = tsl::Identity::FromRandom(random, "Living room: TV");
    CHECK_EQ(id.deviceId, 0x01080f161d242b32ULL);
    CHECK_EQ(id.secret[0], random[8]);
    CHECK_EQ(id.secret[31], random[39]);

    std::string text = id.Serialize();
    CHECK(text.rfind("tsl1:01080f161d242b32:", 0) == 0);
    auto parsed = tsl::Identity::Parse(text + "\n");
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->deviceId, id.deviceId);
        CHECK(parsed->secret == id.secret);
        CHECK_EQ(parsed->name, "Living room: TV");
    }

    CHECK(!tsl::Identity::Parse("").has_value());
    CHECK(!tsl::Identity::Parse("tsl1:zz").has_value());
    CHECK(!tsl::Identity::Parse(text.substr(0, text.size() - 15)).has_value());
    std::string noName = text.substr(0, text.find(':', 22) + 1);
    CHECK(!tsl::Identity::Parse(noName).has_value());

    IHS_ClientConfig config = id.ClientConfig();
    CHECK_EQ(config.deviceId, id.deviceId);
    CHECK(config.secretKey == id.secret.data());
}

void TestPin() {
    CHECK_EQ(tsl::FormatPin(0), "0000");
    CHECK_EQ(tsl::FormatPin(42), "0042");
    CHECK_EQ(tsl::FormatPin(123456789), "6789");
}

void TestKeymap() {
    CHECK_EQ(tsl::ScancodeFromDomCode("KeyA"), 4u);
    CHECK_EQ(tsl::ScancodeFromDomCode("KeyZ"), 29u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Digit1"), 30u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Digit0"), 39u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Enter"), 40u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Escape"), 41u);
    CHECK_EQ(tsl::ScancodeFromDomCode("ArrowUp"), 82u);
    CHECK_EQ(tsl::ScancodeFromDomCode("F12"), 69u);
    CHECK_EQ(tsl::ScancodeFromDomCode("ShiftLeft"), 225u);
    CHECK_EQ(tsl::ScancodeFromDomCode("MetaRight"), 231u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Keya"), 0u);
    CHECK_EQ(tsl::ScancodeFromDomCode("Unidentified"), 0u);
    CHECK_EQ(tsl::ScancodeFromDomCode(""), 0u);
}

void TestGamepadMapping() {
    double axes[4] = {-1.0, 0.5, 1.0, -0.25};
    double values[17] = {0};
    bool pressed[17] = {false};
    pressed[0] = true;   // A
    pressed[4] = true;   // LB
    pressed[9] = true;   // Start
    pressed[12] = true;  // D-pad up
    pressed[16] = true;  // Home
    values[6] = 1.0;     // LT fully pressed
    values[7] = 0.5;     // RT half
    pressed[6] = true;   // triggers are analog, the pressed flag must not set a button bit
    TSL_GamepadState s;
    TSL_GamepadStateFromStandard(axes, 4, values, pressed, 17, &s);
    uint16_t expected = (1u << TSL_PAD_A) | (1u << TSL_PAD_LEFT_SHOULDER) | (1u << TSL_PAD_START) |
                        (1u << TSL_PAD_DPAD_UP) | (1u << TSL_PAD_GUIDE);
    CHECK_EQ(s.buttons, expected);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_LEFT_X], -32768);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_LEFT_Y], 16383);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_RIGHT_X], 32767);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_RIGHT_Y], -8192);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_TRIGGER_LEFT], 32767);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_TRIGGER_RIGHT], 16383);

    // Short arrays (a pad with fewer buttons) leave the rest released.
    TSL_GamepadStateFromStandard(axes, 2, values, pressed, 4, &s);
    CHECK_EQ(s.buttons, static_cast<uint16_t>(1u << TSL_PAD_A));
    CHECK_EQ(s.axes[TSL_PAD_AXIS_RIGHT_X], 0);
    CHECK_EQ(s.axes[TSL_PAD_AXIS_TRIGGER_LEFT], 0);

    // NaN and out of range values are clamped.
    double bad[4] = {0.0 / 0.0, 5.0, -5.0, 0.0};
    TSL_GamepadStateFromStandard(bad, 4, nullptr, nullptr, 0, &s);
    CHECK_EQ(s.axes[0], 0);
    CHECK_EQ(s.axes[1], 32767);
    CHECK_EQ(s.axes[2], -32768);
}

void TestGamepadIds() {
    uint16_t v = 1, p = 1;
    CHECK(TSL_GamepadParseIds("Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02fd)", &v, &p));
    CHECK_EQ(v, 0x045e);
    CHECK_EQ(p, 0x02fd);
    CHECK(TSL_GamepadParseIds("054c-0ce6-DualSense Wireless Controller", &v, &p));
    CHECK_EQ(v, 0x054c);
    CHECK_EQ(p, 0x0ce6);
    CHECK(!TSL_GamepadParseIds("Generic USB Joystick", &v, &p));
    CHECK_EQ(v, 0);
    CHECK_EQ(p, 0);
    CHECK(!TSL_GamepadParseIds("Pad (Vendor: 04", &v, &p));
    CHECK(!TSL_GamepadParseIds(nullptr, &v, &p));

    // Name-only ids, as Samsung TVs report them.
    CHECK(TSL_GamepadGuessIds("DualSense Wireless Controller", &v, &p));
    CHECK_EQ(v, 0x054c);
    CHECK_EQ(p, 0x0ce6);
    CHECK(TSL_GamepadGuessIds("DUALSENSE EDGE WIRELESS CONTROLLER", &v, &p));
    CHECK_EQ(p, 0x0df2);
    CHECK(TSL_GamepadGuessIds("Wireless Controller", &v, &p));
    CHECK_EQ(v, 0x054c);
    CHECK(TSL_GamepadGuessIds("Xbox Wireless Controller", &v, &p));
    CHECK_EQ(v, 0x045e);
    CHECK(!TSL_GamepadGuessIds("Generic USB Joystick", &v, &p));
    CHECK_EQ(v, 0);
    CHECK_EQ(p, 0);
    CHECK(!TSL_GamepadGuessIds(nullptr, &v, &p));

    CHECK_EQ(TSL_GamepadControllerType(0x054c, 0x0ce6), 45);  // PS5
    CHECK_EQ(TSL_GamepadControllerType(0x054c, 0x09cc), 34);  // PS4
    CHECK_EQ(TSL_GamepadControllerType(0x045e, 0x028e), 31);  // Xbox 360
    CHECK_EQ(TSL_GamepadControllerType(0x045e, 0x0b13), 32);  // Xbox One/Series
    CHECK_EQ(TSL_GamepadControllerType(0x057e, 0x2009), 38);  // Switch Pro
    CHECK_EQ(TSL_GamepadControllerType(0x1234, 0x5678), 30);  // unknown: generic
    CHECK_EQ(TSL_GamepadControllerType(0, 0), 30);
}

void TestGamepadReportLayout() {
    TSL_GamepadState s{};
    s.buttons = 0xA55A;
    s.axes[0] = -2;      // 0xFFFE
    s.axes[5] = 0x1234;
    uint8_t report[TSL_GAMEPAD_REPORT_SIZE];
    TSL_GamepadPackReport(&s, 3, report);
    CHECK_EQ(report[0], 0xFE);
    CHECK_EQ(report[1], 0xFF);
    CHECK_EQ(report[10], 0x34);
    CHECK_EQ(report[11], 0x12);
    for (int i = 12; i < 16; i++) CHECK_EQ(report[i], 0);
    CHECK_EQ(report[16], 0x5A);
    CHECK_EQ(report[17], 0xA5);
    CHECK_EQ(report[27], 3);
    for (int i = 28; i < 48; i++) CHECK_EQ(report[i], 0);
}

struct RumbleLog {
    int calls = 0;
    int slot = -1;
    uint16_t low = 0, high = 0;
    uint32_t duration = 0;
};

void RecordRumble(int slot, uint16_t low, uint16_t high, uint32_t duration, void *context) {
    auto *log = static_cast<RumbleLog *>(context);
    log->calls++;
    log->slot = slot;
    log->low = low;
    log->high = high;
    log->duration = duration;
}

void TestGamepadProvider() {
    IHS_HIDProvider *provider = TSL_GamepadProviderCreate();
    IHS_HIDManager *manager = IHS_HIDManagerCreate();
    IHS_HIDManagerAddProvider(manager, provider);

    // Nothing connected: empty enumeration, open fails.
    IHS_Enumeration *e = IHS_HIDProviderEnumerateDevices(provider);
    CHECK_EQ(IHS_EnumerationCount(e), 0u);
    IHS_EnumerationFree(e);
    CHECK(IHS_HIDManagerOpenDevice(manager, "tsl://1") == nullptr);

    TSL_GamepadInfo info{};
    info.vendorId = 0x045e;
    info.productId = 0x02ea;
    std::snprintf(info.name, sizeof(info.name), "Xbox Wireless Controller");
    CHECK(TSL_GamepadProviderSetConnected(provider, 1, true, &info));
    CHECK(!TSL_GamepadProviderSetConnected(provider, 1, true, &info));
    CHECK(!TSL_GamepadProviderSetConnected(provider, 9, true, &info));

    e = IHS_HIDProviderEnumerateDevices(provider);
    CHECK_EQ(IHS_EnumerationCount(e), 1u);
    IHS_EnumerationReset(e);
    IHS_HIDDeviceInfo hid{};
    IHS_HIDProviderDeviceInfo(provider, e, &hid);
    CHECK(hid.path != nullptr && std::strcmp(hid.path, "tsl://1") == 0);
    CHECK(hid.product_string != nullptr && std::strcmp(hid.product_string, "Xbox Wireless Controller") == 0);
    CHECK_EQ(hid.vendor_id, 0x045e);
    CHECK_EQ(hid.product_id, 0x02ea);
    IHS_EnumerationNext(e);
    CHECK(IHS_EnumerationEnded(e));
    IHS_EnumerationFree(e);

    CHECK(!IHS_HIDProviderSupportsDevice(provider, "sdl://1"));
    CHECK(!IHS_HIDProviderSupportsDevice(provider, "tsl://4"));
    CHECK(!IHS_HIDProviderSupportsDevice(provider, "tsl://1x"));
    CHECK(IHS_HIDProviderSupportsDevice(provider, "tsl://0"));

    IHS_HIDManagedDevice *managed = IHS_HIDManagerOpenDevice(manager, "tsl://1");
    CHECK(managed != nullptr);
    if (managed != nullptr) {
        IHS_HIDDevice *device = managed->device;

        // Nothing goes out before Steam starts input reports, even with input pending.
        TSL_GamepadState early{};
        early.buttons = 1u << TSL_PAD_A;
        TSL_GamepadProviderUpdate(provider, 1, &early);
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 0);
        IHS_HIDDeviceUnlock(device);

        // What control_hid.c does for DeviceStartInputReports.
        IHS_HIDReportHolderSetReportLength(&managed->reportHolder, TSL_GAMEPAD_REPORT_SIZE);
        CHECK_EQ(IHS_HIDDeviceStartInputReports(device, TSL_GAMEPAD_REPORT_SIZE), 0);

        // The input held back until now goes out, then nothing while the state stays the same.
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 1);
        CHECK_EQ(device->cls->poll(device), 0);
        IHS_HIDDeviceUnlock(device);

        TSL_GamepadState state{};
        state.buttons = 1u << TSL_PAD_B;
        TSL_GamepadProviderUpdate(provider, 1, &state);
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 1);
        CHECK_EQ(device->cls->poll(device), 0);
        IHS_HIDDeviceUnlock(device);
        CHECK(IHS_HIDReportHolderGetMessage(&managed->reportHolder) != nullptr);

        // Caps feature report: same 20-byte layout as ihslib's SDL backend.
        IHS_Buffer buf;
        IHS_BufferInit(&buf, 64, 64);
        uint8_t number = 0x04;
        CHECK_EQ(IHS_HIDDeviceGetFeatureReport(device, &number, 1, &buf, 64), 0);
        CHECK_EQ(buf.size, 21u);
        const uint8_t *p = IHS_BufferPointer(&buf);
        CHECK_EQ(p[0], 0x04);
        CHECK_EQ(p[1], 1);   // valid
        CHECK_EQ(p[2], 0);   // not xinput
        int32_t type, player;
        std::memcpy(&type, p + 3, 4);
        std::memcpy(&player, p + 7, 4);
        CHECK_EQ(type, 32);  // Xbox One, from the 045e vendor id
        CHECK_EQ(player, 1);
        IHS_BufferClear(&buf, true);

        // Rumble write reaches the callback; requested report version lands in byte 27.
        RumbleLog rumble;
        TSL_GamepadProviderSetRumbleCallback(provider, RecordRumble, &rumble);
        const uint8_t rumbleCmd[9] = {0x01, 0x34, 0x12, 0x78, 0x56, 0xE8, 0x03, 0x00, 0x00};
        CHECK_EQ(IHS_HIDDeviceWrite(device, rumbleCmd, sizeof(rumbleCmd)), 0);
        CHECK_EQ(rumble.calls, 1);
        CHECK_EQ(rumble.slot, 1);
        CHECK_EQ(rumble.low, 0x1234);
        CHECK_EQ(rumble.high, 0x5678);
        CHECK_EQ(rumble.duration, 1000u);
        const uint8_t versionCmd[9] = {0x09, 0x02};
        CHECK_EQ(IHS_HIDDeviceWrite(device, versionCmd, sizeof(versionCmd)), 0);
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 1);  // version byte changed the report
        IHS_HIDDeviceUnlock(device);
        const uint8_t unknownCmd[9] = {0x7f};
        CHECK_EQ(IHS_HIDDeviceWrite(device, unknownCmd, sizeof(unknownCmd)), -1);

        // Unplugged while buttons are held: one delta to a released pad, then quiet. The
        // device stays open until Steam closes it.
        TSL_GamepadState held{};
        held.buttons = 1u << TSL_PAD_Y;
        TSL_GamepadProviderUpdate(provider, 1, &held);
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 1);
        IHS_HIDDeviceUnlock(device);
        CHECK(TSL_GamepadProviderSetConnected(provider, 1, false, nullptr));
        IHS_HIDDeviceLock(device);
        CHECK_EQ(device->cls->poll(device), 1);
        CHECK_EQ(device->cls->poll(device), 0);
        IHS_HIDDeviceUnlock(device);

        // IHS_HIDManagedDeviceClose would notify (and log through) a session, and this
        // manager has none, so do its local half by hand.
        device->cls->close(device);
        managed->closed = true;
        device->managed = nullptr;
        device->cls->free(device);
    }

    IHS_HIDManagerDestroy(manager);
    TSL_GamepadProviderDestroy(provider);
}

class NullSink : public tsl::MediaSink, public tsl::Session::Listener {
public:
    int VideoStart(const IHS_StreamVideoConfig &) override { return 0; }
    IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *, size_t, bool) override { return IHS_StreamVideoSubmitOK; }
    void VideoStop() override {}
    int AudioStart(const IHS_StreamAudioConfig &) override { return 0; }
    int AudioPacket(const uint8_t *, size_t) override { return 0; }
    void AudioStop() override {}
};

void TestSessionLifecycleWithoutConnect() {
    uint8_t random[tsl::Identity::kRandomBytes] = {1};
    tsl::Identity id = tsl::Identity::FromRandom(random, "test");
    IHS_SessionInfo info{};
    IHS_IPAddressFromString(&info.address.ip, "127.0.0.1");
    info.address.port = 27031;
    info.sessionKeyLen = 32;
    NullSink sink;
    {
        tsl::HostKey key{};
        tsl::Session session(id, key, info, tsl::SessionOptions{}, &sink, &sink);
        TSL_GamepadInfo pad{};
        session.SetGamepadConnected(0, true, &pad);
        TSL_GamepadState state{};
        session.UpdateGamepad(0, state);
        CHECK(!session.connected());
        session.SendKey(4, true);  // ignored while not connected
    }
    CHECK(true);
}

// A fake Steam host on localhost: answers one unicast discovery with a status message.
class ProbeListener : public tsl::Client::Listener {
public:
    void OnHostFound(const tsl::Host &host) override {
        std::lock_guard<std::mutex> lock(mutex);
        hosts.push_back(host);
        cond.notify_all();
    }
    std::mutex mutex;
    std::condition_variable cond;
    std::vector<tsl::Host> hosts;
};

void TestProbeAgainstFakeHost() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK_EQ(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t random[tsl::Identity::kRandomBytes] = {9, 9, 9};
    tsl::Identity id = tsl::Identity::FromRandom(random, "probe-test");
    ProbeListener listener;
    tsl::Client client(id, &listener);

    // The client's socket is created on its worker thread; retry until a send succeeds.
    bool sent = false;
    for (int i = 0; i < 50 && !sent; i++) {
        sent = client.Probe("127.0.0.1", port);
        if (!sent) usleep(20000);
    }
    CHECK(sent);
    CHECK(!client.Probe("not-an-ip", port));

    uint8_t packet[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    ssize_t n = recvfrom(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
    CHECK(n > 16);
    if (n > 16) {
        static const uint8_t magic[8] = {0xff, 0xff, 0xff, 0xff, 0x21, 0x4c, 0x5f, 0xa0};
        CHECK(std::memcmp(packet, magic, 8) == 0);
        uint32_t headerLen = packet[8] | (packet[9] << 8) | (packet[10] << 16) | (packet[11] << 24);
        CMsgRemoteClientBroadcastHeader *header =
                cmsg_remote_client_broadcast_header__unpack(nullptr, headerLen, packet + 12);
        CHECK(header != nullptr);
        if (header != nullptr) {
            CHECK_EQ(header->msg_type, k_ERemoteClientBroadcastMsgDiscovery);
            CHECK_EQ(header->client_id, id.deviceId);
            cmsg_remote_client_broadcast_header__free_unpacked(header, nullptr);
        }

        // Reply the way Steam does: header + CMsgRemoteClientBroadcastStatus.
        CMsgRemoteClientBroadcastHeader replyHeader = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
        replyHeader.has_client_id = true;
        replyHeader.client_id = 0x1122334455667788ULL;
        replyHeader.has_msg_type = true;
        replyHeader.msg_type = k_ERemoteClientBroadcastMsgStatus;
        replyHeader.has_instance_id = true;
        replyHeader.instance_id = 42;
        CMsgRemoteClientBroadcastStatus status = CMSG_REMOTE_CLIENT_BROADCAST_STATUS__INIT;
        char hostname[] = "fake-steam-host";
        status.hostname = hostname;
        status.has_ostype = true;
        status.ostype = -184;
        status.has_euniverse = true;
        status.euniverse = 1;
        status.has_games_running = true;
        status.games_running = true;

        std::vector<uint8_t> reply(magic, magic + 8);
        auto appendLE32 = [&](uint32_t v) {
            for (int i = 0; i < 4; i++) reply.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        size_t hlen = cmsg_remote_client_broadcast_header__get_packed_size(&replyHeader);
        appendLE32(static_cast<uint32_t>(hlen));
        size_t off = reply.size();
        reply.resize(off + hlen);
        cmsg_remote_client_broadcast_header__pack(&replyHeader, reply.data() + off);
        size_t blen = cmsg_remote_client_broadcast_status__get_packed_size(&status);
        appendLE32(static_cast<uint32_t>(blen));
        off = reply.size();
        reply.resize(off + blen);
        cmsg_remote_client_broadcast_status__pack(&status, reply.data() + off);
        sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);

        std::unique_lock<std::mutex> lock(listener.mutex);
        listener.cond.wait_for(lock, std::chrono::seconds(2), [&] { return !listener.hosts.empty(); });
        CHECK_EQ(listener.hosts.size(), 1u);
        if (!listener.hosts.empty()) {
            const tsl::Host &host = listener.hosts[0];
            CHECK_EQ(host.hostname(), "fake-steam-host");
            CHECK_EQ(host.address(), "127.0.0.1");
            CHECK_EQ(host.info.address.port, port);
            CHECK_EQ(host.info.clientId, 0x1122334455667788ULL);
            CHECK_EQ(host.info.instanceId, 42u);
            CHECK(host.info.gamesRunning);
        }
    }
    close(fd);
}

// Adds one packet to an ihslib packet window. fragments > 0 makes it the head of a frame
// with that many fragment packets after it; fragment marks one of those.
bool AddPacket(IHS_SessionPacketsWindow *window, uint16_t id, uint32_t millis, int16_t fragments = 0,
               bool fragment = false) {
    IHS_SessionPacket packet{};
    packet.header.type = fragment ? IHS_SessionPacketTypeUnreliableFrag : IHS_SessionPacketTypeUnreliable;
    packet.header.fragmentId = fragments;
    packet.header.packetId = id;
    packet.header.sendTimestamp = IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(millis);
    IHS_BufferInit(&packet.body, 16, 64);
    uint8_t byte = static_cast<uint8_t>(id);
    IHS_BufferAppendMem(&packet.body, &byte, 1);
    bool added = IHS_SessionPacketsWindowAdd(window, &packet);
    IHS_BufferClear(&packet.body, true);
    return added;
}

// Polls one frame; returns the packet id of its head, or -1.
int PollFrame(IHS_SessionPacketsWindow *window) {
    IHS_SessionFrame frame{};
    IHS_BufferInit(&frame.body, 64, 1024);
    int id = IHS_SessionPacketsWindowPoll(window, &frame) ? frame.header.packetId : -1;
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    IHS_BufferClear(&frame.body, true);
    return id;
}

void TestWakePacket() {
    auto packet = tsl::WakePacket("00:1A-2b.3C4d5E");
    CHECK(packet.has_value());
    if (packet) {
        static const uint8_t mac[6] = {0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e};
        for (int i = 0; i < 6; i++) CHECK_EQ((*packet)[i], 0xff);
        bool repeated = true;
        for (int i = 0; i < 16; i++) repeated = repeated && std::memcmp(packet->data() + 6 + i * 6, mac, 6) == 0;
        CHECK(repeated);
    }
    CHECK(!tsl::WakePacket("").has_value());
    CHECK(!tsl::WakePacket("00:1a:2b:3c:4d").has_value());        // 5 bytes
    CHECK(!tsl::WakePacket("00:1a:2b:3c:4d:5e:6f").has_value());  // 7 bytes
    CHECK(!tsl::WakePacket("00:1a:2b:3c:4d:zz").has_value());

    // Whether it arrives depends on the firewall (ufw drops broadcasts to other ports), so
    // only the send is checked here.
    uint8_t random[tsl::Identity::kRandomBytes] = {4};
    tsl::Identity id = tsl::Identity::FromRandom(random, "wake-test");
    ProbeListener listener;
    tsl::Client client(id, &listener);
    CHECK(client.Wake("00:1a:2b:3c:4d:5e", 40009));
    CHECK(!client.Wake("not a mac"));
}

void TestPacketWindowContiguousId() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(16);
    uint16_t id = 0;
    CHECK(!IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK(AddPacket(window, 10, 0));
    CHECK(AddPacket(window, 11, 0));
    CHECK(AddPacket(window, 13, 0));  // 12 lost
    CHECK(AddPacket(window, 14, 0));
    CHECK(IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK_EQ(id, 11);  // what Steam may take as delivered
    CHECK_EQ(PollFrame(window), 10);
    CHECK_EQ(PollFrame(window), 11);
    CHECK_EQ(PollFrame(window), -1);  // waits for 12
    CHECK(IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK_EQ(id, 11);
    CHECK(AddPacket(window, 12, 0));  // the retransmission
    CHECK(IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK_EQ(id, 14);
    CHECK_EQ(PollFrame(window), 12);
    CHECK_EQ(PollFrame(window), 13);
    CHECK_EQ(PollFrame(window), 14);
    CHECK(IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK_EQ(id, 14);
    IHS_SessionPacketsWindowDestroy(window);
}

void TestPacketWindowFullIsNotEmpty() {
    // Head away from slot 0, then fill: the window used to take the last slot and then read
    // as empty, so nothing could be polled or discarded again.
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(8);
    for (uint16_t i = 0; i < 3; i++) CHECK(AddPacket(window, i, 0));
    for (int i = 0; i < 3; i++) CHECK_EQ(PollFrame(window), i);
    CHECK(AddPacket(window, 3, 0, 1));  // frame 3 lost its fragment (4): head stays here
    int added = 0;
    for (uint16_t i = 5; i < 20 && AddPacket(window, i, 0); i++) {
        added++;
        CHECK(IHS_SessionPacketsWindowSize(window) > 0);
    }
    CHECK_EQ(added, 5);  // 3, a free slot for 4, 5..9, and one slot kept free
    CHECK_EQ(IHS_SessionPacketsWindowSize(window), 7);
    IHS_SessionPacketsWindowDestroy(window);
}

void TestPacketWindowDiscardAndReset() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(64);
    const uint32_t discardDiff = IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(200);
    // Frame 0 has two fragments; the second (2) is lost.
    CHECK(AddPacket(window, 0, 0, 2));
    CHECK(AddPacket(window, 1, 0, 0, true));
    // Complete frames every 20 ms after it.
    uint16_t next = 3;
    for (uint32_t ms = 20; ms <= 160; ms += 20) CHECK(AddPacket(window, next++, ms));
    CHECK_EQ(IHS_SessionPacketsWindowDiscard(window, discardDiff), 0);  // not stale yet
    CHECK_EQ(PollFrame(window), -1);  // frame 0 blocks the rest
    for (uint32_t ms = 180; ms <= 260; ms += 20) CHECK(AddPacket(window, next++, ms));
    CHECK(IHS_SessionPacketsWindowDiscard(window, discardDiff) > 0);
    int polled = PollFrame(window);
    CHECK(polled > 0);  // frames after the broken one come through again
    // Reset empties the window and lets it start over from any id.
    IHS_SessionPacketsWindowReset(window);
    uint16_t id = 0;
    CHECK_EQ(IHS_SessionPacketsWindowSize(window), 0);
    CHECK(!IHS_SessionPacketsWindowContiguousId(window, &id));
    CHECK(AddPacket(window, 5000, 1000));
    CHECK_EQ(PollFrame(window), 5000);
    IHS_SessionPacketsWindowDestroy(window);
}

// A gap longer than the window (Wi-Fi gone for a few seconds: 256 packets is 2.6 s of audio)
// used to leave the channel dead. Every later packet was more than the window's capacity ahead
// of the last one seen and was ignored, while the session went on. Add now refuses it, as on an
// overflow, and the data channel starts over from it.
void TestPacketWindowJumpAhead() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(64);
    for (uint16_t id = 0; id < 4; id++) CHECK(AddPacket(window, id, id * 10u));
    for (int id = 0; id < 4; id++) CHECK_EQ(PollFrame(window), id);
    CHECK(!AddPacket(window, 1000, 5000));  // 996 ahead of a 64-packet window
    // What the data channel does when Add refuses a packet.
    IHS_SessionPacketsWindowReset(window);
    CHECK(AddPacket(window, 1000, 5000));
    CHECK(AddPacket(window, 1001, 5010));
    CHECK_EQ(PollFrame(window), 1000);
    CHECK_EQ(PollFrame(window), 1001);
    // A late copy of a packet already passed on is still ignored.
    CHECK(AddPacket(window, 1000, 5000));
    CHECK_EQ(PollFrame(window), -1);
    IHS_SessionPacketsWindowDestroy(window);
}

// Steam sends the stream's colorspace in CStartVideoDataMsg field 6, which ihslib's copy of the
// protocol doesn't know: protobuf-c keeps it as an unknown field, and IHS_UnknownVarint reads it.
void TestUnknownVarint() {
    auto roundTrip = [](uint8_t *varint, size_t len) {
        CStartVideoDataMsg sent = CSTART_VIDEO_DATA_MSG__INIT;
        sent.channel = 4;
        PROTOBUF_C_SET_VALUE(sent, codec, k_EStreamVideoCodecHEVC);
        ProtobufCMessageUnknownField colorspace = {6, PROTOBUF_C_WIRE_TYPE_VARINT, len, varint};
        if (len > 0) {
            sent.base.n_unknown_fields = 1;
            sent.base.unknown_fields = &colorspace;
        }
        std::vector<uint8_t> packed(cstart_video_data_msg__get_packed_size(&sent));
        cstart_video_data_msg__pack(&sent, packed.data());
        CStartVideoDataMsg *got = cstart_video_data_msg__unpack(nullptr, packed.size(), packed.data());
        uint64_t value = got ? IHS_UnknownVarint(&got->base, 6, 99) : 0;
        if (got) cstart_video_data_msg__free_unpacked(got, nullptr);
        return value;
    };
    uint8_t hdr10[] = {5};
    CHECK_EQ(roundTrip(hdr10, sizeof(hdr10)), 5u);  // k_EStreamColorspace_HDR10
    CHECK_EQ(roundTrip(nullptr, 0), 99u);          // not sent: the fallback
    uint8_t twoBytes[] = {0xac, 0x02};
    CHECK_EQ(roundTrip(twoBytes, sizeof(twoBytes)), 300u);
}

void TestPacketWindowSkipLost() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(32);
    CHECK(AddPacket(window, 0, 0, 2));  // frame 0: packets 0, 1, 2; 1 is lost
    CHECK(AddPacket(window, 2, 0, 0, true));
    CHECK_EQ(IHS_SessionPacketsWindowSkipLost(window), 0);  // nothing complete after it yet
    CHECK(AddPacket(window, 3, 20, 1));  // frame 3: packets 3, 4
    CHECK_EQ(IHS_SessionPacketsWindowSkipLost(window), 0);  // frame 3 isn't complete either
    CHECK(AddPacket(window, 4, 20, 0, true));
    CHECK_EQ(IHS_SessionPacketsWindowSkipLost(window), 3);  // drops slots 0..2
    CHECK_EQ(PollFrame(window), 3);
    CHECK(AddPacket(window, 5, 40));
    CHECK_EQ(IHS_SessionPacketsWindowSkipLost(window), 0);  // head is complete: nothing to skip
    CHECK_EQ(PollFrame(window), 5);
    IHS_SessionPacketsWindowDestroy(window);
}

// Steam's video FEC (session/fec.h), encoded here independently: parity row r is the sum of
// d_i / (r ^ (m + i)) over GF(2^8) with polynomial 0x11d.
struct Gf {
    uint8_t exp[512], log[256];
    Gf() {
        unsigned x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
    }
    uint8_t Mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t Inv(uint8_t a) const { return exp[255 - log[a]]; }
};

std::vector<std::vector<uint8_t>> FecShards(const std::vector<uint8_t> &message, uint16_t shardSize, uint16_t m) {
    static const Gf gf;
    size_t k = (message.size() + shardSize - 1) / shardSize;
    std::vector<std::vector<uint8_t>> shards(k + m, std::vector<uint8_t>(shardSize, 0));
    for (size_t i = 0; i < k; i++) {
        size_t n = std::min<size_t>(shardSize, message.size() - i * shardSize);
        std::memcpy(shards[i].data(), message.data() + i * shardSize, n);
    }
    for (size_t r = 0; r < m; r++) {
        for (size_t i = 0; i < k; i++) {
            uint8_t c = gf.Inv(static_cast<uint8_t>(r ^ (m + i)));
            for (size_t j = 0; j < shardSize; j++) shards[k + r][j] ^= gf.Mul(c, shards[i][j]);
        }
    }
    for (auto &shard : shards) {
        uint8_t header[8];
        uint32_t len = static_cast<uint32_t>(message.size());
        std::memcpy(header, &len, 4);
        std::memcpy(header + 4, &shardSize, 2);
        std::memcpy(header + 6, &m, 2);
        shard.insert(shard.begin(), header, header + 8);
    }
    return shards;
}

// Feeds one shard; returns the message if it completed one.
std::vector<uint8_t> FecAdd(IHS_SessionFEC *fec, uint16_t id, int index, const std::vector<uint8_t> &shard,
                            bool *complete) {
    IHS_SessionPacket packet{};
    packet.header.type = IHS_SessionPacketTypeUnreliableFEC;
    packet.header.packetId = id;
    packet.header.fragmentId = static_cast<int16_t>(index);
    IHS_BufferInit(&packet.body, shard.size(), shard.size());
    IHS_BufferAppendMem(&packet.body, shard.data(), shard.size());
    IHS_SessionPacket message{};
    *complete = IHS_SessionFECAdd(fec, &packet, &message);
    IHS_BufferClear(&packet.body, true);
    std::vector<uint8_t> out;
    if (*complete) {
        CHECK_EQ(message.header.type, IHS_SessionPacketTypeUnreliable);
        CHECK_EQ(message.header.fragmentId, 0);
        CHECK_EQ(message.header.packetId, id);
        out.assign(IHS_BufferPointer(&message.body), IHS_BufferPointer(&message.body) + message.body.size);
        IHS_BufferClear(&message.body, true);
    }
    return out;
}

// Sends every shard of a message except the dropped ones; returns how many completed it.
int FecSend(IHS_SessionFEC *fec, uint16_t id, const std::vector<uint8_t> &message, uint16_t shardSize,
            uint16_t m, const std::vector<int> &dropped) {
    auto shards = FecShards(message, shardSize, m);
    int completed = 0;
    for (size_t i = 0; i < shards.size(); i++) {
        if (std::find(dropped.begin(), dropped.end(), static_cast<int>(i)) != dropped.end()) continue;
        bool complete = false;
        auto out = FecAdd(fec, id, static_cast<int>(i), shards[i], &complete);
        if (complete) {
            completed++;
            CHECK(out == message);
        }
    }
    return completed;
}

void TestFec() {
    std::vector<uint8_t> small(5000), large(36399);
    for (size_t i = 0; i < small.size(); i++) small[i] = static_cast<uint8_t>(i * 7 + 3);
    for (size_t i = 0; i < large.size(); i++) large[i] = static_cast<uint8_t>(i * 13 + i / 251);
    IHS_SessionFEC *fec = IHS_SessionFECCreate();
    CHECK_EQ(FecSend(fec, 1, small, 1440, 1, {}), 1);       // 4 data shards, nothing lost
    CHECK_EQ(FecSend(fec, 2, small, 1440, 1, {2}), 1);      // one data shard rebuilt
    CHECK_EQ(FecSend(fec, 3, small, 1440, 1, {3}), 1);      // the short last shard rebuilt
    CHECK_EQ(FecSend(fec, 4, small, 1440, 1, {4}), 1);      // parity lost: data alone
    CHECK_EQ(FecSend(fec, 5, small, 1440, 1, {0, 1}), 0);   // two lost, one parity: gone
    CHECK_EQ(FecSend(fec, 6, large, 1440, 2, {3, 17}), 1);  // 26 data shards, two rebuilt
    CHECK_EQ(FecSend(fec, 7, large, 1440, 2, {25, 26}), 1);  // last data and first parity lost
    CHECK_EQ(FecSend(fec, 8, large, 1440, 2, {0, 9, 27}), 0);
    CHECK_EQ(FecSend(fec, 9, std::vector<uint8_t>(53, 0x41), 64, 1, {0}), 1);  // the parity is a copy

    // A message is out as soon as its data shards are in; later shards change nothing.
    auto shards = FecShards(small, 1440, 1);
    bool complete = false;
    for (int i = 0; i < 4; i++) FecAdd(fec, 10, i, shards[i], &complete);
    CHECK(complete);
    FecAdd(fec, 10, 4, shards[4], &complete);
    CHECK(!complete);
    FecAdd(fec, 10, 1, shards[1], &complete);  // duplicate
    CHECK(!complete);

    // Messages 11..44 in order; a shard of message 11 after that is 33 behind and dropped.
    for (uint16_t id = 11; id <= 44; id++) CHECK_EQ(FecSend(fec, id, small, 1440, 1, {}), 1);
    FecAdd(fec, 11, 0, shards[0], &complete);
    CHECK(!complete);

    IHS_SessionFECStats stats;
    IHS_SessionFECGetStats(fec, &stats);
    CHECK_EQ(stats.recovered, 5u);  // 2, 3, 6, 7, 9
    // Messages 1..12 left the window; 5 and 8 never completed.
    CHECK_EQ(stats.unrecoverable, 2u);
    // Lost shards of the retired messages 1..12: one each in 2, 3, 4 and 9, two in 5, two in 6,
    // two in 7, three in 8.
    CHECK_EQ(stats.shardsLost, 4u + 2u + 2u + 2u + 3u);
    IHS_SessionFECDestroy(fec);
}

// What an audio data channel hands to the app: the first payload byte of each packet.
struct AudioCapture {
    std::mutex mutex;
    std::condition_variable cond;
    std::vector<int> payloads;
};

int CaptureAudioStart(IHS_Session *, const IHS_StreamAudioConfig *, void *) { return 0; }

int CaptureAudioSubmit(IHS_Session *, IHS_Buffer *data, void *context) {
    auto *capture = static_cast<AudioCapture *>(context);
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->payloads.push_back(data->size > 0 ? IHS_BufferPointer(data)[0] : -1);
    capture->cond.notify_all();
    return 0;
}

void CaptureAudioStop(IHS_Session *, void *) {}

// Lost frames go in as -count.
void CaptureAudioLost(IHS_Session *, uint32_t frames, void *context) {
    auto *capture = static_cast<AudioCapture *>(context);
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->payloads.push_back(-static_cast<int>(frames));
}

// The data channel's worker must get past a lost packet. It used to wait behind it until its
// window (256 packets for audio) overflowed, so the packets after a loss came seconds late.
void TestDataChannelGetsPastLostPacket() {
    uint8_t random[tsl::Identity::kRandomBytes] = {2};
    tsl::Identity id = tsl::Identity::FromRandom(random, "test");
    IHS_ClientConfig config = id.ClientConfig();
    tsl::HostKey key{};
    config.secretKey = key.data();
    IHS_SessionInfo info{};
    IHS_IPAddressFromString(&info.address.ip, "127.0.0.1");
    info.address.port = 27031;
    info.sessionKeyLen = 32;
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    AudioCapture capture;
    static const IHS_StreamAudioCallbacks callbacks = {
            .start = CaptureAudioStart,
            .submit = CaptureAudioSubmit,
            .stop = CaptureAudioStop,
            .lost = CaptureAudioLost,
    };
    IHS_SessionSetAudioCallbacks(session, &callbacks, &capture);
    CStartAudioDataMsg start = CSTART_AUDIO_DATA_MSG__INIT;
    start.channel = 3;
    start.codec = k_EStreamAudioCodecOpus;
    start.channels = 2;
    start.frequency = 48000;
    IHS_SessionChannel *channel = IHS_SessionChannelDataAudioCreate(session, &start);

    // Packets 10 ms apart, well inside the 200 ms after which stale packets are discarded.
    // frame: the data frame id, one per audio frame; the packet id counts packets.
    auto send = [&](uint16_t id, uint16_t frame, int16_t fragments, bool fragment) {
        IHS_SessionPacket packet{};
        packet.header.type = fragment ? IHS_SessionPacketTypeUnreliableFrag : IHS_SessionPacketTypeUnreliable;
        packet.header.channelId = static_cast<IHS_SessionChannelId>(start.channel);
        packet.header.fragmentId = fragments;
        packet.header.packetId = id;
        packet.header.sendTimestamp = IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(1000 + id * 10);
        // k_EStreamDataPacket, the 12-byte data frame header, then the payload: the packet id.
        uint8_t body[1 + 12 + 1] = {k_EStreamDataPacket, static_cast<uint8_t>(frame), static_cast<uint8_t>(frame >> 8)};
        body[13] = static_cast<uint8_t>(id);
        IHS_BufferInit(&packet.body, sizeof(body), sizeof(body));
        IHS_BufferAppendMem(&packet.body, body, sizeof(body));
        IHS_SessionChannelDataReceived(channel, &packet);
        IHS_BufferClear(&packet.body, true);
    };
    send(0, 0, 0, false);
    send(1, 1, 2, false);  // frame 1, packets 1, 2 and 3; 2 is lost
    send(3, 1, 0, true);
    send(4, 2, 0, false);
    // packet 5, frame 3, is lost
    for (uint16_t id = 6; id < 10; id++) send(id, static_cast<uint16_t>(id - 2), 0, false);
    send(10, 3, 0, false);  // frame 3 after all, too late: dropped
    {
        std::unique_lock<std::mutex> lock(capture.mutex);
        capture.cond.wait_for(lock, std::chrono::seconds(1), [&] { return capture.payloads.size() >= 8; });
        // Each lost frame is reported where it belongs, before the frame after it.
        CHECK(capture.payloads == (std::vector<int>{0, -1, 4, -1, 6, 7, 8, 9}));
    }
    IHS_SessionChannelStop(channel);
    IHS_SessionChannelDestroy(channel);
    IHS_SessionDestroy(session);
}

// The host silence check reads the clock, then the last packet's time. A packet arriving in
// between can be 1 ms newer than the clock reading: that is no silence, not 4294967295 ms of it,
// which used to end live streams.
void TestSilenceMs() {
    CHECK_EQ(IHS_SessionSilenceMs(1000, 1001), -1);
    CHECK(IHS_SessionSilenceMs(1000, 1001) < 10000);
    CHECK_EQ(IHS_SessionSilenceMs(12000, 1000), 11000);
    CHECK_EQ(IHS_SessionSilenceMs(5, 0xFFFFFFF0u), 21);  // the 32-bit millisecond clock wraps
}

// Steam's MTU pings arrive as unconnected packets: [u8 type][u32 size][CDiscoveryPingRequest].
// A size near 4 GB used to pass the bounds check on 32 bits (offset + size wrapped around), and
// packet_size_requested went to the padding unchecked. The session isn't run, so its send queue
// shows whether a ping got an answer.
void TestUnconnectedPingSizes() {
    uint8_t random[tsl::Identity::kRandomBytes] = {3};
    tsl::Identity id = tsl::Identity::FromRandom(random, "test");
    IHS_ClientConfig config = id.ClientConfig();
    tsl::HostKey key{};
    config.secretKey = key.data();
    IHS_SessionInfo info{};
    IHS_IPAddressFromString(&info.address.ip, "127.0.0.1");
    info.address.port = 27031;
    info.sessionKeyLen = 32;
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    IHS_SessionChannel *discovery = IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery);
    // wrap: claim a message size near 4 GB instead of the real one.
    auto receive = [&](bool wrap, uint32_t requested) {
        CDiscoveryPingRequest ping = CDISCOVERY_PING_REQUEST__INIT;
        ping.has_sequence = 1;
        ping.sequence = 1;
        ping.has_packet_size_requested = 1;
        ping.packet_size_requested = requested;
        uint8_t message[32];
        size_t messageLen = cdiscovery_ping_request__pack(&ping, message);
        uint32_t size = wrap ? 0xFFFFFFFCu : static_cast<uint32_t>(messageLen);
        IHS_SessionPacket packet{};
        packet.header.type = IHS_SessionPacketTypeUnconnected;
        packet.header.channelId = IHS_SessionChannelIdDiscovery;
        uint8_t prefix[5] = {k_EStreamDiscoveryPingRequest, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                             static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)};
        // Exactly the packet's size, so that reading past it is past the allocation.
        IHS_BufferInit(&packet.body, sizeof(prefix) + messageLen, sizeof(prefix) + messageLen);
        IHS_BufferAppendMem(&packet.body, prefix, sizeof(prefix));
        IHS_BufferAppendMem(&packet.body, message, messageLen);
        IHS_SessionChannelReceivedPacket(discovery, &packet);
        IHS_BufferClear(&packet.body, true);
    };
    receive(true, 512);  // size past the packet: ignored, without reading past it
    CHECK(IHS_QueueIsEmpty(session->sendQueue));
    receive(false, 0xFFFFFFFFu);  // asks for 4 GB of padding: ignored
    CHECK(IHS_QueueIsEmpty(session->sendQueue));
    receive(false, 512);  // a real probe size gets its answer
    CHECK(!IHS_QueueIsEmpty(session->sendQueue));
    IHS_SessionDestroy(session);
}

// Steam sends Opus with codec_data of length 0, which used to reach memcpy as a null pointer.
void TestAudioEmptyCodecData() {
    uint8_t random[tsl::Identity::kRandomBytes] = {4};
    tsl::Identity id = tsl::Identity::FromRandom(random, "test");
    IHS_ClientConfig config = id.ClientConfig();
    tsl::HostKey key{};
    config.secretKey = key.data();
    IHS_SessionInfo info{};
    IHS_IPAddressFromString(&info.address.ip, "127.0.0.1");
    info.address.port = 27031;
    info.sessionKeyLen = 32;
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    CStartAudioDataMsg start = CSTART_AUDIO_DATA_MSG__INIT;
    start.channel = 3;
    start.codec = k_EStreamAudioCodecOpus;
    start.channels = 2;
    start.frequency = 48000;
    start.has_codec_data = 1;  // empty: data is NULL, len 0
    IHS_SessionChannel *channel = IHS_SessionChannelDataAudioCreate(session, &start);
    CHECK(channel != nullptr);
    IHS_SessionChannelStop(channel);
    IHS_SessionChannelDestroy(channel);
    // The microphone channel starts from the same message.
    channel = IHS_SessionChannelDataMicrophoneCreate(session, &start);
    CHECK(channel != nullptr);
    IHS_SessionChannelDestroy(channel);
    IHS_SessionDestroy(session);
}

// A reliable packet waits for its ACK, and its timer sends it again after 10 ms. On Wi-Fi the
// round trip is about that long, so an ACK often lands while the timer fires; with a controller
// sending 250 reliable reports a second that happens constantly. Cancelling used to free the
// pending packet on the receive thread while the timer's end callback freed it on the timer
// thread, then stop a timer task that was already gone (a use-after-free under ASan).
void TestRetransmissionCancelRacesTimer() {
    uint8_t random[tsl::Identity::kRandomBytes] = {3};
    tsl::Identity id = tsl::Identity::FromRandom(random, "test");
    IHS_ClientConfig config = id.ClientConfig();
    tsl::HostKey key{};
    config.secretKey = key.data();
    IHS_SessionInfo info{};
    IHS_IPAddressFromString(&info.address.ip, "127.0.0.1");
    info.address.port = 27031;
    info.sessionKeyLen = 32;
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    IHS_SessionRetransmission *retransmission = &session->retransmission;
    auto queue = [&](uint16_t packetId) {
        IHS_SessionPacket packet{};
        packet.header.type = IHS_SessionPacketTypeReliable;
        packet.header.channelId = IHS_SessionChannelIdControl;
        packet.header.packetId = packetId;
        packet.header.sendTimestamp = IHS_SessionPacketTimestamp();
        IHS_SessionPacketBodyInitialize(&packet.body, false);
        const uint8_t payload[1] = {0};
        IHS_BufferAppendMem(&packet.body, payload, sizeof(payload));
        bool queued = IHS_RetransmissionQueue(retransmission, &packet);
        IHS_SessionPacketClear(&packet, true);
        return queued;
    };
    // Nothing sends here (the session isn't connected), so a resend stays in the send queue.
    auto resent = [&] {
        IHS_MutexLock(session->sendQueueMutex);
        bool any = !IHS_QueueIsEmpty(session->sendQueue);
        IHS_MutexUnlock(session->sendQueueMutex);
        return any;
    };

    // Acknowledged well before its timer: cancelled once, and never sent again.
    CHECK(queue(1));
    CHECK(IHS_RetransmissionCancel(retransmission, IHS_SessionChannelIdControl, 1, 0));
    CHECK(!IHS_RetransmissionCancel(retransmission, IHS_SessionChannelIdControl, 1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(!resent());
    // Not acknowledged: sent again when the timer fires.
    CHECK(queue(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(resent());

    // ACKs around the 10 ms mark, from several threads at once. CHECK isn't thread-safe, so
    // the threads count what failed.
    std::atomic<int> notQueued{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; i++) {
                auto packetId = static_cast<uint16_t>(1000 + t * 1000 + i);
                if (!queue(packetId)) notQueued++;
                std::this_thread::sleep_for(std::chrono::microseconds(9000 + (i % 21) * 100));
                IHS_RetransmissionCancel(retransmission, IHS_SessionChannelIdControl, packetId, 0);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    CHECK_EQ(notQueued.load(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    IHS_SessionDestroy(session);
}

// The statistics counters, without the i64.atomic instructions that hang Tizen 5.5: writers
// and a reader at once, across the 32-bit boundary, so 32-bit storage can't pass. CHECK isn't
// thread-safe, so the reader only records what it saw.
void TestCounter64() {
    const uint64_t initial = (uint64_t{1} << 32) - 10;
    tsl::Counter64 counter(initial);
    std::atomic<bool> done{false};
    bool monotonic = true;
    std::thread reader([&] {
        uint64_t previous = initial;
        while (!done) {
            uint64_t current = counter.load();
            if (current < previous) monotonic = false;
            previous = current;
        }
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; t++) {
        writers.emplace_back([&] {
            for (int i = 0; i < 50000; i++) counter += 3;
        });
    }
    for (auto &thread : writers) thread.join();
    done = true;
    reader.join();
    uint64_t expected = initial + 4 * 50000 * 3;
    CHECK(monotonic);
    CHECK_EQ(counter.load(), expected);
    CHECK_EQ(counter++, expected);
    CHECK_EQ(++counter, expected + 2);
}

}  // namespace

int main() {
    tsl::SetLogLevel(IHS_LogLevelWarn);
    IHS_Init();
    TestIdentityRoundTrip();
    TestPin();
    TestKeymap();
    TestGamepadMapping();
    TestGamepadIds();
    TestGamepadReportLayout();
    TestGamepadProvider();
    TestSessionLifecycleWithoutConnect();
    TestProbeAgainstFakeHost();
    TestPacketWindowContiguousId();
    TestPacketWindowFullIsNotEmpty();
    TestPacketWindowDiscardAndReset();
    TestPacketWindowSkipLost();
    TestPacketWindowJumpAhead();
    TestUnknownVarint();
    TestFec();
    TestDataChannelGetsPastLostPacket();
    TestRetransmissionCancelRacesTimer();
    TestSilenceMs();
    TestUnconnectedPingSizes();
    TestAudioEmptyCodecData();
    TestWakePacket();
    TestCounter64();
    IHS_Quit();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
