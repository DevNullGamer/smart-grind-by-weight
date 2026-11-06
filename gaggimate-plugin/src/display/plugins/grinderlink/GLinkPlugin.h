#ifndef GLINKPLUGIN_H
#define GLINKPLUGIN_H

// GrinderLink plugin: GaggiMate as the BLE central of a Smart Grind By Weight grinder (GLink v1,
// docs/GLINK-PROTOCOL.md). Device code only: the whole file is empty in the simulator build.
#ifndef GAGGIMATE_SIM

#include "GLinkClient.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <deque>
#include <display/core/Plugin.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Controller;
class PluginManager;
class WebSocketHandler;

// Threads: WebSocket requests arrive on the AsyncTCP task and only enqueue a Command. Everything else (the NimBLE
// calls, the glinkc::Client, the replies and events) runs on the main loop() task. NimBLE callbacks (host task) only set
// flags, collect scan hits or enqueue a notification packet. Nothing is replayed after a reconnect: a command that is
// queued or in flight when the link drops is answered with DISCONNECTED and forgotten.
class GLinkPlugin : public Plugin, private glinkc::Sink, private glinkc::Listener {
  public:
    GLinkPlugin();
    ~GLinkPlugin() override;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    enum class CmdKind : uint8_t {
        STATUS,
        SCAN,
        SELECT_DEVICE,
        FORGET,
        CONNECT,
        DISCONNECT,
        PROFILE_SELECT,
        PROFILE_UPDATE,
        START,
        STOP,
        RESULT_GET
    };

    // One WebSocket request, copied out of the JSON document on the AsyncTCP task.
    struct Command {
        CmdKind kind = CmdKind::STATUS;
        uint32_t clientId = 0;
        uint32_t epoch = 0; // linkEpoch when queued: a command from before a reconnect is refused (STALE)
        String type; // the request type, the reply is "res:" + type.substring(4)
        String rid;
        String address; // SELECT_DEVICE: "aa:bb:cc:dd:ee:ff"
        String name;
        uint8_t addrType = 0;
        int profile = -1;
        bool hasDose = false;
        float dose = 0.0f;
        bool hasSize = false;
        bool sizeNull = false;
        float size = 0.0f;
        bool keepalive = false; // CONNECT: only refresh the idle timer, never make a link
        String badField;        // present with the wrong type, or missing: INVALID_PARAM
        bool hasUnit = false;
        bool unitNull = false;
        String unit;
    };

    // The request that is on the wire (the Client allows one) and who gets its answer.
    struct Inflight {
        bool valid = false;
        bool internal = false; // plugin-initiated (profile or result refresh): nobody to answer
        glinkc::RequestKind kind = glinkc::RequestKind::NONE;
        Command cmd;
    };

    // Fixed buffers: the scan callback runs on the NimBLE task and stores a hit without allocating.
    struct Found {
        char address[18] = {};
        uint8_t type = 0;
        char name[33] = {}; // printable ASCII only
        int rssi = 0;
    };

    enum class LinkPhase : uint8_t { IDLE, CONNECTING, LINKED };

    struct ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
        explicit ScanCallbacks(GLinkPlugin &owner) : owner(owner) {}
        void onResult(NimBLEAdvertisedDevice *device) override;
        GLinkPlugin &owner;
    };

    struct LinkCallbacks : public NimBLEClientCallbacks {
        explicit LinkCallbacks(GLinkPlugin &owner) : owner(owner) {}
        void onDisconnect(NimBLEClient *) override;
        GLinkPlugin &owner;
    };

    struct RxPacket {
        uint16_t len = 0;
        uint8_t data[GLINK_MAX_PACKET_BYTES];
    };

    // glinkc::Sink
    bool write_packet(const uint8_t *packet, size_t len) override;

    // glinkc::Listener
    void on_ready() override;
    void on_base_info(const glinkc::BaseInfo &) override;
    void on_period(const glinkc::PeriodInfo &period, bool profileRevChanged) override;
    void on_result(const glinkc::ResultInfo &) override;
    void on_profiles(const glinkc::ProfileEntry *, int) override;
    void on_response(glinkc::RequestKind request, const glinkc::Response &response) override;
    void on_request_timeout(glinkc::RequestKind request) override;
    void on_stop_reply(bool accepted) override;
    void on_stop_timeout() override;
    void on_start_outcome(bool started, const char *devState, const char *blocked) override;
    void on_must_disconnect(const char *reason) override;

    // WebSocket side (AsyncTCP task)
    void registerWsHandlers(WebSocketHandler *ws);
    void enqueue(CmdKind kind, uint32_t clientId, JsonDocument &request);

    // Main loop side
    void handle(const Command &cmd, uint32_t now);
    void startScan(const Command &cmd, uint32_t now);
    void finishScan();
    void selectDevice(const Command &cmd);
    void forget(const Command &cmd);
    bool connectLink(uint32_t now);
    void dropLink(const char *reason, bool userRequested);
    void drainRx(uint32_t now);
    void autoFetch(uint32_t now);
    void updateIdle(uint32_t now);
    bool grinderBusy() const;
    bool ready() const { return client != nullptr && client->state() == glinkc::LinkState::READY; }
    bool clientBusy() const { return inflight.valid || (client != nullptr && client->pending() != glinkc::RequestKind::NONE); }
    void beginClientOp(const Command &cmd, uint32_t now);
    void setInflight(const Command &cmd, glinkc::RequestKind kind);
    void failInflight(const char *code, const char *message);

    // Replies and events
    void replyOk(const Command &cmd, bool withStatus);
    void replyError(const Command &cmd, const char *code, const char *message = nullptr, const char *field = nullptr);
    void replyResponse(const Command &cmd, const glinkc::Response &response);
    void fillStatus(JsonObject out);
    void publishState(uint32_t now);
    void setError(const char *text);

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    WebSocketHandler *ws = nullptr;

    // Command queue (AsyncTCP task -> loop task)
    std::mutex cmdMutex;
    std::deque<Command> commands;

    // Notification queue (NimBLE host task -> loop task); allocated in setup()
    std::mutex rxMutex;
    std::unique_ptr<RxPacket[]> rxRing;
    size_t rxHead = 0;
    size_t rxCount = 0;
    std::atomic<bool> rxOverflow{false};
    std::unique_ptr<RxPacket> scratch; // the packet being handed to the Client

    // Scan hits (NimBLE host task -> loop task)
    std::mutex foundMutex;
    Found found[8];
    size_t foundCount = 0;
    std::atomic<bool> scanning{false};
    uint32_t scanDeadline = 0;
    ScanCallbacks scanCallbacks;

    // Link
    LinkCallbacks linkCallbacks;
    NimBLEClient *ble = nullptr;
    NimBLERemoteCharacteristic *rx = nullptr;
    NimBLERemoteCharacteristic *tx = nullptr;
    LinkPhase phase = LinkPhase::IDLE;
    std::atomic<bool> linkLost{false};
    std::atomic<uint32_t> linkEpoch{0}; // changes on every connect and every drop
    uint32_t lastConnectAttempt = 0;
    uint32_t lastUse = 0;
    bool connectAttempted = false;
    bool disconnectWanted = false;
    String disconnectReason;

    // Session (allocated in setup(), about 3 KB plus the parse target; see the README on stack use)
    std::unique_ptr<glinkc::Client> client;
    Inflight inflight;
    uint32_t lastAutoFetch = 0;
    Command stopCmd;
    bool stopWaiting = false;

    // What the card shows
    glinkc::PeriodInfo shownPeriod;
    bool hasShownPeriod = false;
    String startOutcome; // "", "started", "not_started", "unknown" (timeout: watch periodInfo)
    String startNote;
    bool startTimedOut = false;
    bool stopUnconfirmed = false;
    char lastError[64] = {};
    bool dirty = false;
    uint32_t lastEvent = 0;
    bool stackLogged = false;
};

#endif // GAGGIMATE_SIM
#endif // GLINKPLUGIN_H
