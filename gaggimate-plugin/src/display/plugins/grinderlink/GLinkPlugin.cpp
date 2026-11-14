#ifndef GAGGIMATE_SIM

#include "GLinkPlugin.h"

#include "bluetooth/glink_disconnect.h"
#include <esp_bt.h>
#include <nimble/nimble/host/include/host/ble_hs.h>
#include <cmath>
#include <display/core/Controller.h>
#include <display/core/constants.h>
#include <display/core/Event.h>
#include <display/core/PluginManager.h>
#include <display/plugins/WebSocketHandler.h>
#include <display/util/PsramAllocator.h>

// The display is a NimBLE central of the controller and of a scale; the grinder is the third link.
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 3, "GrinderLink needs CONFIG_BT_NIMBLE_MAX_CONNECTIONS of at least 3");

namespace {
constexpr const char *TAG = "GLinkPlugin";
constexpr uint32_t SCAN_WINDOW_MS = 6000;         // one Scan press listens this long
constexpr uint32_t CONNECT_RETRY_MS = 3000;       // minimum gap between two connect attempts
constexpr uint8_t CONNECT_TIMEOUT_S = 5;          // manual Connect; NimBLE's default of 30 s would block the main loop
constexpr uint8_t AUTO_CONNECT_TIMEOUT_S = 3;     // automatic reconnect attempts block the loop task less
constexpr uint32_t EVENT_MIN_INTERVAL_MS = 250;   // state events to the browser: 4 Hz at most
constexpr uint32_t AUTO_FETCH_MIN_INTERVAL_MS = 2000;
// Notification ring between the NimBLE task and loop(). One packet takes 2 + length bytes: 8 KB hold about 370 packets
// of 20 bytes (the grinder fragments to 20 bytes while it does not know the MTU) or 33 of 244. The loop task can be
// busy for a few hundred ms (a connect, the web socket), and the grinder sends up to 100 packets per second.
constexpr size_t RX_RING_BYTES = 8192;
constexpr size_t RX_DRAIN_PER_LOOP = 64;
constexpr size_t MAX_FOUND = 8;
constexpr size_t MAX_COMMANDS = 8;
constexpr int NO_REASON = -1;
constexpr uint16_t NO_LINK_HANDLE = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE
constexpr uint32_t RSSI_LOG_MS = 5000;      // the link's RSSI is logged this often while linked (chg023 1b)
constexpr uint32_t RSSI_WATCH_MS = 1000;    // and read this often while a web page is connected (the card shows it)

// Registered with NimBLE in connectLink(); the stack keeps the pointer, so it lives for the whole run.
ble_gap_event_listener gapListener;

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// "aa:bb:cc:dd:ee:ff"
bool validAddress(const char *a) {
    if (a == nullptr || strlen(a) != 17)
        return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (a[i] != ':')
                return false;
        } else if (!isHex(a[i])) {
            return false;
        }
    }
    return true;
}

// Stored form: "aa:bb:cc:dd:ee:ff" (public address) or "aa:bb:cc:dd:ee:ff/r" (random address).
bool parseStored(const String &stored, std::string &address, uint8_t &type) {
    if (stored.length() != 17 && stored.length() != 19)
        return false;
    String addr = stored.substring(0, 17);
    if (!validAddress(addr.c_str()))
        return false;
    type = BLE_ADDR_PUBLIC;
    if (stored.length() == 19) {
        if (stored.substring(17) != "/r")
            return false;
        type = BLE_ADDR_RANDOM;
    }
    address = addr.c_str();
    return true;
}

String storedForm(const std::string &address, uint8_t type) {
    String s = address.c_str();
    if (type == BLE_ADDR_RANDOM)
        s += "/r";
    return s;
}

bool startsWith(const std::string &text, const char *prefix) { return text.rfind(prefix, 0) == 0; }

int32_t tenths(float value) { return static_cast<int32_t>(lroundf(value * 10.0f)); }

double fromTenths(int32_t value) { return static_cast<double>(value) / 10.0; }

bool isGrindingState(const char *devState) { return strcmp(devState, "GRINDING") == 0 || strcmp(devState, "STARTING") == 0; }

bool periodDiffers(const glinkc::PeriodInfo &a, const glinkc::PeriodInfo &b) {
    return strcmp(a.dev_state, b.dev_state) != 0 || a.weight_valid != b.weight_valid || a.weight_tenths != b.weight_tenths ||
           a.select_profile != b.select_profile || strcmp(a.phase, b.phase) != 0 || a.has_target != b.has_target ||
           a.target_tenths != b.target_tenths || strcmp(a.mode, b.mode) != 0 || a.has_lead_in != b.has_lead_in ||
           a.lead_in_ms != b.lead_in_ms || strcmp(a.blocked, b.blocked) != 0 || a.result_seq != b.result_seq ||
           a.profile_rev != b.profile_rev;
}

void putResult(JsonObject o, const glinkc::ResultInfo &r) {
    o["seq"] = r.seq;
    o["code"] = String(r.code);
    o["origin"] = String(r.origin);
    o["profile"] = r.profile;
    o["mode"] = String(r.mode);
    o["target"] = fromTenths(r.target_tenths);
    o["finalWeight"] = fromTenths(r.final_weight_tenths);
    o["settled"] = r.weight_settled;
    o["pulses"] = r.pulses;
    o["durationMs"] = r.duration_ms;
    o["motorMs"] = r.motor_ms;
}
} // namespace

GLinkPlugin::GLinkPlugin() : scanCallbacks(*this), linkCallbacks(*this) {}

GLinkPlugin::~GLinkPlugin() { dropLink("", true); }

void GLinkPlugin::setup(Controller *_controller, PluginManager *_pluginManager) {
    controller = _controller;
    pluginManager = _pluginManager;

    // The session and the packet ring are the plugin's big objects; they live on the heap, never on a task stack.
    client.reset(new (std::nothrow) glinkc::Client(*this, *this));
    rxStorage.reset(new (std::nothrow) uint8_t[RX_RING_BYTES]);
    scratch.reset(new (std::nothrow) RxPacket());
    if (rxStorage)
        rxRing.init(rxStorage.get(), RX_RING_BYTES);
    if (!client || !rxStorage || !scratch) {
        ESP_LOGE(TAG, "Out of memory, GrinderLink is off");
        client.reset();
        rxRing.init(nullptr, 0);  // no dangling pointer to the storage released next
        rxStorage.reset();
        scratch.reset();
        return;
    }

    pluginManager->on("webui:ws:register", [this](Event const &event) { registerWsHandlers(event.getPointer<WebSocketHandler>("ws")); });
    // Sketch for the GaggiMate maintainer: GaggiMate does not own a dose, so nothing is sent to the grinder from here.
    // The prototype is driven from the settings card. A dose source and the exact wiring are for the maintainer to decide.
    pluginManager->on("controller:grind:start", [](Event const &) { ESP_LOGI(TAG, "GaggiMate grind started (no grinder action)"); });
    pluginManager->on("controller:grind:end", [](Event const &) { ESP_LOGI(TAG, "GaggiMate grind ended (no grinder action)"); });
    // May run on the UI, button or AsyncTCP task: only latch STANDBY, loop() consumes it (a short standby between two
    // loop passes must still clear the user's Disconnect and arm a reconnect).
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        if (event.getInt("value") == MODE_STANDBY)
            standbyLatch = true;
    });
}

//------------------------------------------------------------------------------
// WebSocket side (AsyncTCP task): copy the request, queue it, nothing else
//------------------------------------------------------------------------------

void GLinkPlugin::registerWsHandlers(WebSocketHandler *socket) {
    if (socket == nullptr)
        return;
    ws = socket;
    static const struct {
        const char *type;
        CmdKind kind;
    } requests[] = {
        {"req:grinderlink:status", CmdKind::STATUS},
        {"req:grinderlink:scan", CmdKind::SCAN},
        {"req:grinderlink:select", CmdKind::SELECT_DEVICE},
        {"req:grinderlink:forget", CmdKind::FORGET},
        {"req:grinderlink:connect", CmdKind::CONNECT},
        {"req:grinderlink:disconnect", CmdKind::DISCONNECT},
        {"req:grinderlink:profile:select", CmdKind::PROFILE_SELECT},
        {"req:grinderlink:profile:update", CmdKind::PROFILE_UPDATE},
        {"req:grinderlink:start", CmdKind::START},
        {"req:grinderlink:stop", CmdKind::STOP},
        {"req:grinderlink:result", CmdKind::RESULT_GET},
    };
    for (const auto &r : requests) {
        const CmdKind kind = r.kind;
        ws->registerRequestHandler(r.type, [this, kind](uint32_t clientId, JsonDocument &request) { enqueue(kind, clientId, request); });
    }
    ESP_LOGI(TAG, "WebSocket requests registered");
}

void GLinkPlugin::enqueue(CmdKind kind, uint32_t clientId, JsonDocument &request) {
    Command cmd;
    cmd.kind = kind;
    cmd.clientId = clientId;
    cmd.epoch = linkEpoch.load();
    cmd.type = request["tp"].as<String>();
    cmd.rid = request["rid"].as<String>();
    cmd.address = request["address"].as<String>();
    cmd.name = request["name"].as<String>();
    cmd.addrType = request["addrType"].as<int>() == BLE_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    cmd.keepalive = request["keepalive"].as<bool>();
    cmd.profile = request["profile"].is<int>() ? request["profile"].as<int>() : -1;
    auto present = [&](const char *key) { return request[key].is<JsonVariant>(); };
    if (present("dose")) {
        if (request["dose"].is<float>()) {
            cmd.hasDose = true;
            cmd.dose = request["dose"].as<float>();
        } else {
            cmd.badField = "dose";
        }
    }
    if (present("grindSize")) {
        if (request["grindSize"].is<float>()) {
            cmd.hasSize = true;
            cmd.size = request["grindSize"].as<float>();
        } else if (request["grindSize"].isNull()) { // present and null: clear
            cmd.hasSize = true;
            cmd.sizeNull = true;
        } else {
            cmd.badField = "grindSize";
        }
    }
    if (present("grindUnit")) {
        if (request["grindUnit"].is<const char *>()) {
            cmd.hasUnit = true;
            cmd.unit = request["grindUnit"].as<String>();
        } else if (request["grindUnit"].isNull()) {
            cmd.hasUnit = true;
            cmd.unitNull = true;
        } else {
            cmd.badField = "grindUnit";
        }
    }
    if (kind == CmdKind::SELECT_DEVICE && cmd.name.isEmpty())
        cmd.badField = "name";

    std::lock_guard<std::mutex> lock(cmdMutex);
    if (commands.size() >= MAX_COMMANDS && kind != CmdKind::STOP) {
        // Answer on the spot; the queue is full because the loop task is stuck in a connect.
        if (ws != nullptr) {
            JsonDocument resp(&psramAllocator);
            resp["tp"] = String("res:") + cmd.type.substring(4);
            resp["rid"] = cmd.rid;
            resp["ok"] = false;
            resp["error"] = "BUSY";
            ws->sendJson(clientId, resp);
        }
        return;
    }
    if (kind == CmdKind::STOP)
        commands.push_front(std::move(cmd)); // a stop goes ahead of everything that waits
    else
        commands.push_back(std::move(cmd));
}

//------------------------------------------------------------------------------
// Main loop
//------------------------------------------------------------------------------

void GLinkPlugin::loop() {
    if (!client)
        return;
    const uint32_t now = millis();
    if (!grinderStoredKnown)
        refreshGrinderStored(); // the settings are loaded before the first loop() pass

    if (scanning && static_cast<int32_t>(now - scanDeadline) >= 0)
        finishScan();

    // Each lost link is reported to the reconnect policy exactly once (note_link_lost).
    if (rxOverflow.exchange(false) && phase == LinkPhase::LINKED) {
        ESP_LOGW(TAG, "Notification queue overflow, dropping the link");
        dropLink("notification queue overflow", false);
        reconnect_.note_link_lost(now);
    }
    if (linkLost.exchange(false) && phase == LinkPhase::LINKED) {
        logLostReason();
        dropLink("link lost", false);
        reconnect_.note_link_lost(now);
    }

    if (phase == LinkPhase::LINKED) {
        drainRx(now);
        client->tick(now);
        if (disconnectWanted) {
            disconnectWanted = false;
            dropLink(disconnectReason.c_str(), false);
            reconnect_.note_link_lost(now);
        }
    }

    // One command per pass keeps a blocking connect from starving the notification queue.
    Command cmd;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(cmdMutex);
        if (!commands.empty()) {
            cmd = std::move(commands.front());
            commands.pop_front();
            have = true;
        }
    }
    if (have)
        handle(cmd, now);
    else
        autoReconnect(now); // not in the pass that handled a command: it may have changed the link or blocked for seconds

    applyPendingDose(millis()); // every pass: a pending dose expires even while the machine is busy

    if (phase == LinkPhase::LINKED) {
        logLinkRssi(now);
        autoFetch(now);
    }
    publishState(now);
}

void GLinkPlugin::handle(const Command &cmd, uint32_t now) {
    if (cmd.badField.length() > 0) {
        replyError(cmd, "INVALID_PARAM", "missing or wrong type", cmd.badField.c_str());
        return;
    }
    switch (cmd.kind) {
    case CmdKind::STATUS:
        replyOk(cmd, true);
        break;
    case CmdKind::SCAN:
        startScan(cmd, now);
        break;
    case CmdKind::SELECT_DEVICE:
        selectDevice(cmd);
        break;
    case CmdKind::FORGET:
        forget(cmd);
        break;
    case CmdKind::CONNECT:
        if (phase != LinkPhase::LINKED && !cmd.keepalive)
            reconnect_.note_grinder_changed(now); // a Connect press clears the user's Disconnect, even if answered BUSY
        if (phase == LinkPhase::LINKED) {
            replyOk(cmd, true);
        } else if (cmd.keepalive) {
            // A keep-alive never makes a link: it may have been sent just before the link was lost.
            replyError(cmd, "NOT_CONNECTED", "the link ended; press Connect");
        } else if (gaggimateBusy()) {
            // A connect blocks loop() for seconds; not while a grind or brew runs (the scale heartbeat lives on this task).
            replyError(cmd, "BUSY", "GaggiMate is busy; connect when it is idle");
        } else {
            const bool linked = connectLink(now, CONNECT_TIMEOUT_S, true);
            if (linked)
                reconnect_.note_connected(millis());
            else
                reconnect_.note_attempt(millis(), false);
            auto answer = [&](const Command &q) {
                if (linked)
                    replyOk(q, true);
                else
                    replyError(q, "CONNECT_FAILED", lastError[0] != '\0' ? lastError : nullptr);
            };
            answer(cmd);
            // Further connects queued behind this one get the same answer instead of another blocking attempt.
            std::deque<Command> same;
            {
                std::lock_guard<std::mutex> lock(cmdMutex);
                for (auto it = commands.begin(); it != commands.end();) {
                    if (it->kind == CmdKind::CONNECT && !it->keepalive) {
                        same.push_back(std::move(*it));
                        it = commands.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const auto &q : same)
                answer(q);
        }
        break;
    case CmdKind::DISCONNECT:
        dropLink("", true);
        reconnect_.note_user_disconnect(); // stays off until STANDBY, a restart, or a select/forget/Connect
        replyOk(cmd, true);
        break;
    case CmdKind::STOP:
        if (phase != LinkPhase::LINKED) {
            replyError(cmd, "NOT_CONNECTED");
        } else if (stopWaiting) {
            replyError(cmd, "BUSY", "a stop is already waiting for the grinder");
        } else if (!client->stop(now)) {
            replyError(cmd, "NOT_CONNECTED");
        } else {
            stopCmd = cmd;
            stopWaiting = true;
        }
        break;
    default:
        beginClientOp(cmd, now);
        break;
    }
}

void GLinkPlugin::beginClientOp(const Command &cmd, uint32_t now) {
    if (!ready()) {
        replyError(cmd, "NOT_CONNECTED", "connect first; nothing is replayed after a reconnect");
        return;
    }
    if (cmd.epoch != linkEpoch.load()) { // queued before the link was made or lost: never replayed
        replyError(cmd, "STALE", "the link changed after this request was made");
        return;
    }
    if (clientBusy()) {
        replyError(cmd, "BUSY", "another request is waiting for the grinder");
        return;
    }
    bool sent = false;
    glinkc::RequestKind kind = glinkc::RequestKind::NONE;
    switch (cmd.kind) {
    case CmdKind::PROFILE_SELECT:
        kind = glinkc::RequestKind::PROFILE_SELECT;
        sent = client->profile_select(cmd.profile, now);
        break;
    case CmdKind::PROFILE_UPDATE: {
        kind = glinkc::RequestKind::PROFILE_UPDATE;
        glinkc::ProfileUpdate update;
        if (cmd.hasDose) {
            update.has_dose = true;
            update.dose_tenths = tenths(cmd.dose);
        }
        if (cmd.hasSize) {
            update.has_size = true;
            update.size_null = cmd.sizeNull;
            update.size_tenths = cmd.sizeNull ? 0 : tenths(cmd.size);
        }
        if (cmd.hasUnit) {
            update.has_unit = true;
            update.unit_null = cmd.unitNull;
            if (!cmd.unitNull) {
                if (cmd.unit.length() > GLINK_GRIND_UNIT_MAX_LEN) {
                    replyError(cmd, "INVALID_PARAM", "grindUnit is too long", "grindUnit");
                    return;
                }
                strncpy(update.unit, cmd.unit.c_str(), sizeof(update.unit) - 1);
            }
        }
        sent = client->profile_update(cmd.profile, update, now);
        break;
    }
    case CmdKind::START: {
        kind = glinkc::RequestKind::START;
        // Always send expectDose: the dose of the request, else the one cached from the grinder for that profile.
        bool haveDose = false;
        int32_t expect = 0;
        if (cmd.hasDose) {
            haveDose = true;
            expect = tenths(cmd.dose);
        } else {
            for (int i = 0; i < client->profile_count(); i++) {
                if (client->profiles()[i].index == cmd.profile) {
                    haveDose = true;
                    expect = client->profiles()[i].dose_tenths;
                }
            }
        }
        if (!haveDose) {
            replyError(cmd, "NO_DOSE", "no dose known for this profile; read the profiles first", "expectDose");
            return;
        }
        startOutcome = "";
        startNote = "";
        startTimedOut = false;
        sent = client->start(cmd.profile, expect, now);
        break;
    }
    case CmdKind::RESULT_GET:
        kind = glinkc::RequestKind::RESULT_GET;
        sent = client->result_get(now);
        break;
    default:
        break;
    }
    if (!sent) {
        // The Client refuses what the grinder would refuse, and a start between a start timeout and the next periodInfo.
        replyError(cmd, "REFUSED_LOCALLY", "the request was not sent (values out of range, or wait for the next status)");
        return;
    }
    setInflight(cmd, kind);
}

void GLinkPlugin::setInflight(const Command &cmd, glinkc::RequestKind kind) {
    inflight.valid = true;
    inflight.internal = false;
    inflight.kind = kind;
    inflight.cmd = cmd;
}

void GLinkPlugin::failInflight(const char *code, const char *message) {
    if (inflight.valid && !inflight.internal)
        replyError(inflight.cmd, code, message);
    inflight.valid = false;
    inflight.cmd = Command();
    if (stopWaiting) {
        replyError(stopCmd, code, message);
        stopWaiting = false;
    }
}

// The plugin reads the profiles when profileRev changed and the last result when resultSeq is newer.
void GLinkPlugin::autoFetch(uint32_t now) {
    if (!ready() || clientBusy() || now - lastAutoFetch < AUTO_FETCH_MIN_INTERVAL_MS)
        return;
    glinkc::RequestKind kind = glinkc::RequestKind::NONE;
    if (client->needs_profile_refresh() && client->profile_get(-1, now))
        kind = glinkc::RequestKind::PROFILE_GET;
    else if (client->needs_result_fetch() && client->result_get(now))
        kind = glinkc::RequestKind::RESULT_GET;
    if (kind == glinkc::RequestKind::NONE)
        return;
    lastAutoFetch = now;
    inflight.valid = true;
    inflight.internal = true;
    inflight.kind = kind;
}

// Loop task, once per pass (chg024 part B). The only place that writes the selected profile's dose; the listener callbacks
// only record into doseSave. The process lock is taken here and never while cmdMutex, rxMutex or foundMutex is held (loop()
// holds none of them at this point). Like Smart Dose the write is memory-only: it is lost on a profile reselect or reboot.
void GLinkPlugin::applyPendingDose(uint32_t now) {
    if (!doseSave.has_pending())
        return; // cheap path: no lock, no settings read
    Settings &settings = controller->getSettings();
    if (!settings.isGrinderLinkSaveDose()) {
        doseSave.cancel();
        dirty = true;
        return;
    }
    int32_t doseTenths = 0;
    uint32_t seq = 0;
    bool taken = false;
    {
        std::lock_guard<std::recursive_mutex> lock(controller->getProcessLock());
        Profile &profile = controller->getProfileManager()->getSelectedProfile();
        const bool canApply = controller->getMode() == MODE_BREW && !controller->isActive() && !controller->isUpdating() &&
                              !controller->isAutotuning() && !profile.utility;
        taken = doseSave.take(canApply, now, &doseTenths, &seq);
        if (taken) {
            const float dose = static_cast<float>(doseTenths) / 10.0f;
            if (fabsf(profile.dose - dose) >= 0.001f) // compare-first: an equal dose is not written again
                profile.dose = dose;
        }
    }
    if (taken) {
        // Integers only: float printf is stack-hungry and the loop task has little headroom. Tenths are 50..400 here.
        ESP_LOGI(TAG, "Dose set to %d.%d g from grind #%u", static_cast<int>(doseTenths / 10), static_cast<int>(doseTenths % 10),
                 static_cast<unsigned>(seq));
    }
    if (taken || !doseSave.has_pending())
        dirty = true; // written or expired: the card's note changed (a value that keeps waiting changes nothing)
}

// A connect blocks loop() for seconds, so it never starts while a grind or brew runs or the system is not ready.
bool GLinkPlugin::gaggimateBusy() const { return controller->isActive() || controller->getSystemState() != SYSTEM_READY; }

// The settings string is a heap copy: read it when the choice changes, never per loop pass.
void GLinkPlugin::refreshGrinderStored() {
    std::string address;
    uint8_t type = BLE_ADDR_PUBLIC;
    grinderStored = parseStored(controller->getSettings().getGrinderLinkAddress(), address, type);
    grinderStoredKnown = true;
}

// Loop task, every pass without a command (chg023 part 2). The policy decides when; this only connects. The automatic path
// calls connectLink() and nothing else: a start is only ever sent by beginClientOp().
void GLinkPlugin::autoReconnect(uint32_t now) {
    glinkc::GaggimateState state = glinkc::GaggimateState::IDLE;
    if (standbyLatch.exchange(false) || controller->getMode() == MODE_STANDBY)
        state = glinkc::GaggimateState::STANDBY;
    else if (gaggimateBusy())
        state = glinkc::GaggimateState::BUSY;
    const glinkc::Inputs inputs{grinderStored, phase == LinkPhase::LINKED, scanning.load(), state};
    if (reconnect_.poll(inputs, now) != glinkc::Action::ATTEMPT)
        return;
    // Ignored (the policy's safety deadline retries later) while a connect is pending or the machine got busy meanwhile.
    if (phase != LinkPhase::IDLE || gaggimateBusy())
        return;
    if (connectLink(now, AUTO_CONNECT_TIMEOUT_S, false)) {
        reconnect_.note_connected(millis());
        if (!autoReconnectLogged) {
            autoReconnectLogged = true;
            ESP_LOGI(TAG, "Reconnected automatically; stack high-water mark of the loop task: %u bytes free at least",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
    } else {
        reconnect_.note_attempt(millis(), false); // the connect blocked: the backoff counts from its end
    }
}

//------------------------------------------------------------------------------
// Device choice, scan
//------------------------------------------------------------------------------

// Runs on the NimBLE task. The hit is stored in fixed buffers; the library itself returns std::string copies of the name and
// address (one short allocation each), which cannot be avoided in NimBLE-Arduino 1.4. Names are cut to printable ASCII
// before they can reach the web page.
void GLinkPlugin::ScanCallbacks::onResult(NimBLEAdvertisedDevice *device) {
    if (!owner.scanning || device == nullptr || !device->haveName())
        return;
    const std::string name = device->getName();
    if (!startsWith(name, BLE_DEVICE_NAME))
        return;
    const std::string address = device->getAddress().toString();
    if (address.size() != 17)
        return;
    std::lock_guard<std::mutex> lock(owner.foundMutex);
    for (size_t i = 0; i < owner.foundCount; i++) {
        if (strcmp(owner.found[i].address, address.c_str()) == 0) {
            owner.found[i].rssi = device->getRSSI();
            return;
        }
    }
    if (owner.foundCount >= MAX_FOUND)
        return;
    Found &f = owner.found[owner.foundCount++];
    memcpy(f.address, address.c_str(), 17);
    f.address[17] = '\0';
    f.type = device->getAddressType();
    f.rssi = device->getRSSI();
    size_t n = 0;
    for (char ch : name) {
        if (n + 1 >= sizeof(f.name))
            break;
        const unsigned char u = static_cast<unsigned char>(ch);
        f.name[n++] = (u >= 0x20 && u < 0x7f) ? ch : '?';
    }
    f.name[n] = '\0';
}

void GLinkPlugin::LinkCallbacks::onDisconnect(NimBLEClient *) { owner.linkLost = true; }

// NimBLE host task. The client callback above does not receive the reason, so the GAP listener keeps it: for this link
// (linkDownReason) and as the latest disconnect of any connection (lastDisconnect, for a drop inside connect()).
// No logging and no allocation here: the loop task logs it (logLostReason(), logConnectFailure()).
int GLinkPlugin::onGapEvent(struct ble_gap_event *event, void *arg) {
    if (event->type != BLE_GAP_EVENT_DISCONNECT)
        return 0;
    GLinkPlugin *self = static_cast<GLinkPlugin *>(arg);
    const uint16_t handle = event->disconnect.conn.conn_handle;
    const int reason = event->disconnect.reason;
    self->lastDisconnect = (static_cast<uint32_t>(handle) << 16) | (static_cast<uint32_t>(reason) & 0xFFFF);
    self->disconnectSeq++;
    if (handle == self->linkHandle.load())
        self->linkDownReason = reason;
    return 0;
}

// Loop task: the reason the stack reported for the link that was just lost (the callback and the listener run on the
// same disconnect, so the listener's value can be missing when the loop task gets there first)
void GLinkPlugin::logLostReason() {
    const int reason = linkDownReason.exchange(NO_REASON);
    if (reason == NO_REASON) {
        ESP_LOGW(TAG, "Grinder link lost, the stack reported no reason");
        return;
    }
    char text[48];
    glink_disconnect_describe(text, sizeof(text), reason);
    ESP_LOGW(TAG, "Grinder link lost, %s", text);
}

// Loop task, while LINKED: reads the RSSI of the grinder link (every RSSI_WATCH_MS while a web page is connected, else every
// RSSI_LOG_MS), caches it for the card and logs it every RSSI_LOG_MS (chg023 1b). getRssi() returns 0 when the read fails
// (NimBLE-Arduino 1.4 NimBLEClient::getRssi): that clears the cached value and is not logged, a real reading is never
// 0 dBm. A wrapped millis() does not matter (signed difference).
void GLinkPlugin::logLinkRssi(uint32_t now) {
    if (ble == nullptr || static_cast<int32_t>(now - rssiDue) < 0)
        return;
    rssiDue = now + (ws != nullptr && ws->hasClients() ? RSSI_WATCH_MS : RSSI_LOG_MS);
    const int rssi = ble->getRssi();
    if (rssi != linkRssi) {
        linkRssi = rssi;
        dirty = true;
    }
    if (rssi == 0 || static_cast<int32_t>(now - rssiLogDue) < 0)
        return;
    rssiLogDue = now + RSSI_LOG_MS;
    char text[16];
    glink_format_dbm(text, sizeof(text), rssi);
    ESP_LOGI(TAG, "Grinder link RSSI %s", text);
}

// Loop task, a connect or setup that failed: names the grinder's own drop when the stack reported one, else the latest
// disconnect of any connection since the attempt began (its handle is named, because the grinder's handle is not known
// before connect() returns).
void GLinkPlugin::logConnectFailure(uint32_t seqAtAttempt) {
    char text[48];
    const int reason = linkDownReason.exchange(NO_REASON);
    if (reason != NO_REASON) {
        glink_disconnect_describe(text, sizeof(text), reason);
        ESP_LOGW(TAG, "Grinder link ended during connect, %s", text);
        return;
    }
    if (disconnectSeq.load() == seqAtAttempt)
        return;
    const uint32_t last = lastDisconnect.load();
    glink_disconnect_describe(text, sizeof(text), static_cast<int>(last & 0xFFFF));
    ESP_LOGW(TAG, "Grinder connect failed, last disconnect on conn handle %u, %s", static_cast<unsigned>(last >> 16), text);
}

void GLinkPlugin::startScan(const Command &cmd, uint32_t now) {
    if (phase != LinkPhase::IDLE) {
        replyError(cmd, "BUSY", "disconnect before scanning");
        return;
    }
    if (!NimBLEDevice::getInitialized() || !controller->getClientController()->isConnected()) {
        // BleClientTransport (the controller link) shares the scan singleton and needs it to find the controller again.
        replyError(cmd, "BLE_NOT_READY", "scan only while the controller link is up");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(foundMutex);
        foundCount = 0;
    }
    // The scale plugin owns this scan singleton. Taking it over for one window is safe: its maintainScan() sees the scan
    // stopped when the window ends and restarts it with its own callbacks. Not run against a real scale yet (README).
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->setMaxResults(0);
    scan->setDuplicateFilter(false);
    scanning = true;
    scanDeadline = now + SCAN_WINDOW_MS;
    if (!scan->start(0, nullptr, false)) {
        scanning = false;
        replyError(cmd, "SCAN_FAILED");
        return;
    }
    dirty = true;
    replyOk(cmd, true);
}

void GLinkPlugin::finishScan() {
    scanning = false;
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    dirty = true;
}

void GLinkPlugin::selectDevice(const Command &cmd) {
    if (!validAddress(cmd.address.c_str())) {
        replyError(cmd, "INVALID_PARAM", "address must look like aa:bb:cc:dd:ee:ff", "address");
        return;
    }
    if (scanning)
        finishScan();
    std::string address = cmd.address.c_str();
    for (auto &c : address)
        c = static_cast<char>(tolower(c));
    dropLink("", true);
    doseSave.on_link_down(); // a new choice starts without a baseline or pending dose
    Settings &settings = controller->getSettings();
    settings.setGrinderLinkAddress(storedForm(address, cmd.addrType));
    settings.setGrinderLinkName(cmd.name.substring(0, 32));
    refreshGrinderStored();
    reconnect_.note_grinder_changed(millis()); // a new choice clears the user's Disconnect and connects at once
    lastError[0] = '\0';
    connectAttempted = false;
    dirty = true;
    replyOk(cmd, true);
}

void GLinkPlugin::forget(const Command &cmd) {
    dropLink("", true);
    doseSave.on_link_down();
    Settings &settings = controller->getSettings();
    settings.setGrinderLinkAddress("");
    settings.setGrinderLinkName("");
    refreshGrinderStored();
    reconnect_.note_grinder_changed(millis());
    lastError[0] = '\0';
    connectAttempted = false;
    hasShownPeriod = false;
    startOutcome = "";
    dirty = true;
    replyOk(cmd, true);
}

//------------------------------------------------------------------------------
// Link
//------------------------------------------------------------------------------

void GLinkPlugin::setError(const char *text) {
    snprintf(lastError, sizeof(lastError), "%s", text);
    dirty = true;
}

bool GLinkPlugin::connectLink(uint32_t now, uint8_t timeoutSeconds, bool manual) {
    std::string address;
    uint8_t type = BLE_ADDR_PUBLIC;
    if (!parseStored(controller->getSettings().getGrinderLinkAddress(), address, type)) {
        setError("no grinder selected");
        return false;
    }
    if (!NimBLEDevice::getInitialized()) {
        setError("Bluetooth is not ready");
        return false;
    }
    if (scanning)
        finishScan();
    if (connectAttempted && now - lastConnectAttempt < CONNECT_RETRY_MS) {
        if (lastError[0] == '\0')
            setError("waiting before the next connect attempt");
        return false;
    }
    connectAttempted = true;
    // The throttle counts from the END of the attempt (a connect can block for seconds).
    struct StampAtExit {
        uint32_t &t;
        ~StampAtExit() { t = millis(); }
    } stampAtExit{lastConnectAttempt};
    if (NimBLEDevice::getClientListSize() >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        setError("no free Bluetooth connection (controller, scale and grinder use all three)");
        return false;
    }

    // The reason of a drop comes from onGapEvent(); BLE_HS_EALREADY is the answer after the first connect
    const int listenerRc = ble_gap_event_listener_register(&gapListener, &GLinkPlugin::onGapEvent, this);
    if (listenerRc != 0 && listenerRc != BLE_HS_EALREADY)
        ESP_LOGW(TAG, "Disconnect reasons will not be logged (listener rc %d)", listenerRc);
    linkDownReason = NO_REASON;
    const uint32_t seqAtAttempt = disconnectSeq.load();

    lastError[0] = '\0';
    phase = LinkPhase::CONNECTING;
    ble = NimBLEDevice::createClient(NimBLEAddress(address, type));
    if (ble == nullptr) {
        phase = LinkPhase::IDLE;
        setError("could not create the Bluetooth client");
        return false;
    }
    ble->setClientCallbacks(&linkCallbacks, false);
    ble->setConnectTimeout(timeoutSeconds);
    linkLost = false;

    const char *failure = nullptr;
    if (!ble->connect()) {
        failure = "could not connect to the grinder";
    } else {
        linkHandle = ble->getConnId();
        NimBLERemoteService *service = ble->getService(NimBLEUUID(GLINK_SERVICE_UUID));
        rx = service != nullptr ? service->getCharacteristic(NimBLEUUID(GLINK_RX_CHAR_UUID)) : nullptr;
        tx = service != nullptr ? service->getCharacteristic(NimBLEUUID(GLINK_TX_CHAR_UUID)) : nullptr;
        if (rx == nullptr || tx == nullptr || !rx->canWrite() || !tx->canNotify()) {
            failure = "the device has no GLink service (old grinder firmware?)";
        } else {
            {
                std::lock_guard<std::mutex> lock(rxMutex);
                rxRing.clear();
            }
            rxOverflow = false;
            // Subscribe before the hello: the first answer is a notification.
            const bool subscribed = tx->subscribe(
                true,
                [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
                    if (len == 0)
                        return;
                    if (len > GLINK_MAX_PACKET_BYTES) { // cannot be a GLink packet: drop the link
                        rxOverflow = true;
                        return;
                    }
                    std::lock_guard<std::mutex> lock(rxMutex);
                    if (!rxRing.push(data, len)) {
                        rxOverflow = true;
                        return;
                    }
                },
                true);
            if (!subscribed)
                failure = "could not subscribe to the grinder";
        }
    }
    if (failure != nullptr) {
        ESP_LOGW(TAG, "%s", failure);
        logConnectFailure(seqAtAttempt); // before dropLink(): it clears linkHandle
        dropLink(failure, false); // cleans up whatever exists
        return false;
    }

    const uint16_t mtu = ble->getMTU();
    const size_t packet = mtu > 23 ? std::min<size_t>(mtu - 3, GLINK_MAX_PACKET_BYTES) : GLINK_DEFAULT_PACKET_BYTES;
    phase = LinkPhase::LINKED;
    linkEpoch++;
    hasShownPeriod = false;
    if (manual) { // an automatic reconnect keeps the "No start was seen" warning
        startOutcome = "";
        startTimedOut = false;
        stopUnconfirmed = false;
    }
    ESP_LOGI(TAG, "Linked to %s, packet size %u", address.c_str(), static_cast<unsigned>(packet));
    // NimBLEDevice::getPower() returns -128 for levels outside its 8-level table: ask the controller and decode it.
    int txDbm = 0;
    txPowerKnown = glink_tx_power_level_dbm(static_cast<int>(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)), &txDbm);
    txPowerDbm = txPowerKnown ? txDbm : 0;
    if (txPowerKnown) {
        char txText[16];
        glink_format_dbm(txText, sizeof(txText), txDbm);
        ESP_LOGI(TAG, "TX power %s", txText);
    } else {
        ESP_LOGI(TAG, "TX power unknown");
    }
    linkRssi = 0;
    rssiDue = now; // read at once
    rssiLogDue = now + RSSI_LOG_MS;
    if (!client->on_connected(packet, now)) {
        logConnectFailure(seqAtAttempt);
        dropLink("the hello could not be written", false);
        return false;
    }
    dirty = true;
    return true;
}

// Ends the link and everything tied to it. Queued commands stay queued only if they are not tied to a request on the wire;
// nothing that was waiting for an answer is replayed.
void GLinkPlugin::dropLink(const char *reason, bool userRequested) {
    doseSave.on_link_down(); // the baseline belongs to this link; a result that arrives during a gap is never written
    if (client)
        client->on_disconnected();
    if (ble != nullptr) {
        if (ble->isConnected())
            ble->disconnect();
        if (!NimBLEDevice::deleteClient(ble))
            ESP_LOGW(TAG, "deleteClient failed; the client object is leaked");
        ble = nullptr;
    }
    rx = nullptr;
    tx = nullptr;
    linkHandle = NO_LINK_HANDLE;
    linkRssi = 0;
    txPowerDbm = 0;
    txPowerKnown = false;
    const bool wasLinked = phase != LinkPhase::IDLE;
    phase = LinkPhase::IDLE;
    linkEpoch++;
    linkLost = false;
    disconnectWanted = false;
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        rxRing.clear();
    }
    if (inflight.valid && !inflight.internal && inflight.kind == glinkc::RequestKind::START)
        startTimedOut = true; // the start may have reached the grinder: outcome unknown
    failInflight("DISCONNECTED", "the link to the grinder ended");
    if (startTimedOut)
        startOutcome = "unknown";
    if (!userRequested && reason != nullptr && reason[0] != '\0')
        setError(reason);
    if (wasLinked) {
        hasShownPeriod = false;
        dirty = true;
    }
}

void GLinkPlugin::drainRx(uint32_t now) {
    for (size_t i = 0; i < RX_DRAIN_PER_LOOP; i++) {
        if (rxOverflow.load()) // checked before every packet: a gap must not reach the reassembler
            return;
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            if (rxRing.count() == 0)
                return;
            scratch->len = static_cast<uint16_t>(rxRing.pop(scratch->data, sizeof(scratch->data)));
        }
        if (scratch->len == 0)
            continue;
        client->on_notify(scratch->data, scratch->len, now);
        if (phase != LinkPhase::LINKED) // on_must_disconnect() may have ended the link
            return;
    }
}

bool GLinkPlugin::write_packet(const uint8_t *packet, size_t len) {
    if (ble == nullptr || rx == nullptr || !ble->isConnected())
        return false;
    return rx->writeValue(packet, len, true);
}

//------------------------------------------------------------------------------
// glinkc::Listener (always called from inside drainRx(), tick() or one of the Client calls on the loop task)
//------------------------------------------------------------------------------

void GLinkPlugin::on_ready() {
    ESP_LOGI(TAG, "Grinder ready");
    dirty = true;
    doseSave.on_ready(client->period().result_seq); // the Client stored the first periodInfo before this call
    if (!stackLogged) {
        stackLogged = true;
        ESP_LOGI(TAG, "Stack high-water mark of the loop task: %u bytes free at least",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
}

void GLinkPlugin::on_base_info(const glinkc::BaseInfo &) { dirty = true; }

void GLinkPlugin::on_period(const glinkc::PeriodInfo &period, bool) {
    if (!hasShownPeriod || periodDiffers(shownPeriod, period)) {
        shownPeriod = period;
        hasShownPeriod = true;
        dirty = true;
    }
    // Spec 7.5: after a start or stop timeout the periodInfo says what happened.
    if (startTimedOut) {
        startTimedOut = false;
        startOutcome = isGrindingState(period.dev_state) ? "started" : "not_seen";
        dirty = true;
    }
    if (stopUnconfirmed && !isGrindingState(period.dev_state)) {
        stopUnconfirmed = false;
        dirty = true;
    }
}

// Listener callback on the loop task: it only records (no profile, controller or NVS access beyond one setting read);
// applyPendingDose() decides and writes. weight_settled is spec 5.6: true exactly for OK, OVERSHOOT and MAX_PULSES.
void GLinkPlugin::on_result(const glinkc::ResultInfo &result) {
    dirty = true;
    doseSave.on_result(result.seq, result.weight_settled, result.final_weight_tenths,
                       controller->getSettings().isGrinderLinkSaveDose(), millis());
    if (ws == nullptr || !ws->hasClients())
        return;
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:grinderlink:result";
    putResult(doc["result"].to<JsonObject>(), result);
    ws->broadcastJson(doc);
}

void GLinkPlugin::on_profiles(const glinkc::ProfileEntry *, int) { dirty = true; }

void GLinkPlugin::on_response(glinkc::RequestKind request, const glinkc::Response &response) {
    dirty = true;
    if (!inflight.valid || inflight.kind != request)
        return;
    if (!inflight.internal)
        replyResponse(inflight.cmd, response);
    if (request == glinkc::RequestKind::START && response.success)
        startOutcome = "pending";
    inflight.valid = false;
    inflight.cmd = Command();
}

void GLinkPlugin::on_request_timeout(glinkc::RequestKind request) {
    dirty = true;
    if (request == glinkc::RequestKind::START)
        startTimedOut = true; // the Client refuses a new start until a periodInfo has been seen
    if (!inflight.valid || inflight.kind != request)
        return;
    if (!inflight.internal) {
        replyError(inflight.cmd, "TIMEOUT",
                   request == glinkc::RequestKind::START ? "no answer to the start; watch the status before trying again"
                                                         : "no answer from the grinder");
    }
    inflight.valid = false;
    inflight.cmd = Command();
}

void GLinkPlugin::on_stop_reply(bool accepted) {
    dirty = true;
    if (!stopWaiting)
        return;
    if (accepted)
        replyOk(stopCmd, false);
    else
        replyError(stopCmd, "REFUSED", "the grinder did not accept the stop");
    stopWaiting = false;
}

void GLinkPlugin::on_stop_timeout() {
    dirty = true;
    stopUnconfirmed = true; // cleared when a periodInfo shows the grinder is no longer grinding
    if (!stopWaiting)
        return;
    replyError(stopCmd, "TIMEOUT", "no answer to the stop; watch the status");
    stopWaiting = false;
}

void GLinkPlugin::on_start_outcome(bool started, const char *devState, const char *blocked) {
    dirty = true;
    startOutcome = started ? "started" : "not_started";
    startNote = String(devState != nullptr ? devState : "");
    if (blocked != nullptr && blocked[0] != '\0') {
        startNote += "/";
        startNote += blocked;
    }
}

void GLinkPlugin::on_must_disconnect(const char *reason) {
    disconnectWanted = true;
    disconnectReason = reason != nullptr ? reason : "protocol error";
}

//------------------------------------------------------------------------------
// Replies and events (loop task)
//------------------------------------------------------------------------------

void GLinkPlugin::replyOk(const Command &cmd, bool withStatus) {
    if (ws == nullptr)
        return;
    JsonDocument doc(&psramAllocator);
    doc["tp"] = String("res:") + cmd.type.substring(4);
    doc["rid"] = cmd.rid;
    doc["ok"] = true;
    if (withStatus)
        fillStatus(doc.as<JsonObject>());
    ws->sendJson(cmd.clientId, doc);
}

void GLinkPlugin::replyError(const Command &cmd, const char *code, const char *message, const char *field) {
    if (ws == nullptr)
        return;
    JsonDocument doc(&psramAllocator);
    doc["tp"] = String("res:") + cmd.type.substring(4);
    doc["rid"] = cmd.rid;
    doc["ok"] = false;
    doc["error"] = String(code);
    if (message != nullptr)
        doc["message"] = String(message);
    if (field != nullptr)
        doc["field"] = String(field);
    ws->sendJson(cmd.clientId, doc);
}

void GLinkPlugin::replyResponse(const Command &cmd, const glinkc::Response &response) {
    if (!response.success) {
        replyError(cmd, response.failure.code, response.failure.message[0] != '\0' ? response.failure.message : nullptr,
                   response.failure.field[0] != '\0' ? response.failure.field : nullptr);
        return;
    }
    if (ws == nullptr)
        return;
    JsonDocument doc(&psramAllocator);
    doc["tp"] = String("res:") + cmd.type.substring(4);
    doc["rid"] = cmd.rid;
    doc["ok"] = true;
    if (response.has_lead_in)
        doc["leadInMs"] = response.lead_in_ms;
    if (response.has_select)
        doc["selectProfile"] = response.select_profile;
    if (response.has_update_flags) {
        doc["changed"] = response.changed;
        doc["stored"] = response.stored;
    }
    fillStatus(doc.as<JsonObject>());
    ws->sendJson(cmd.clientId, doc);
}

void GLinkPlugin::fillStatus(JsonObject out) {
    Settings &settings = controller->getSettings();
    out["active"] = true;
    out["address"] = settings.getGrinderLinkAddress();
    out["name"] = settings.getGrinderLinkName();
    const char *link = "idle";
    if (scanning)
        link = "scanning";
    else if (phase == LinkPhase::CONNECTING)
        link = "connecting";
    else if (phase == LinkPhase::LINKED)
        link = ready() ? "ready" : "linked";
    out["link"] = link;
    if (lastError[0] != '\0')
        out["error"] = String(lastError);
    out["startOutcome"] = startOutcome;
    if (startNote.length() > 0)
        out["startNote"] = startNote;
    out["stopUnconfirmed"] = stopUnconfirmed;
    if (doseSave.last_saved_tenths() != 0)
        out["doseSaved"] = fromTenths(doseSave.last_saved_tenths()); // last dose written by this plugin, grams
    const char *doseNote = nullptr;
    if (doseSave.has_pending()) {
        doseNote = "waiting";
    } else {
        switch (doseSave.last_skip()) {
        case glinkc::DoseSkip::FEATURE_OFF: doseNote = "disabled"; break;
        case glinkc::DoseSkip::NOT_SETTLED: doseNote = "not_settled"; break;
        case glinkc::DoseSkip::OUT_OF_RANGE: doseNote = "out_of_range"; break;
        default: break;
        }
    }
    if (doseNote != nullptr)
        out["doseNote"] = doseNote;

    JsonArray list = out["found"].to<JsonArray>();
    {
        std::lock_guard<std::mutex> lock(foundMutex);
        for (size_t i = 0; i < foundCount; i++) {
            const Found &f = found[i];
            JsonObject o = list.add<JsonObject>();
            o["address"] = String(f.address);
            o["addrType"] = f.type;
            o["name"] = String(f.name);
            o["rssi"] = f.rssi;
        }
    }

    if (!client || phase != LinkPhase::LINKED)
        return;
    if (linkRssi != 0)
        out["rssi"] = linkRssi;
    if (txPowerKnown)
        out["txPower"] = txPowerDbm;
    if (client->has_base()) {
        const glinkc::BaseInfo &b = client->base();
        JsonObject o = out["device"].to<JsonObject>();
        o["device"] = String(b.device);
        o["fw"] = String(b.fw_version);
        o["board"] = String(b.board);
        o["proto"] = b.proto;
        o["doseMin"] = fromTenths(b.dose_min_tenths);
        o["doseMax"] = fromTenths(b.dose_max_tenths);
    }
    if (client->has_period()) {
        const glinkc::PeriodInfo &p = client->period();
        JsonObject o = out["period"].to<JsonObject>();
        o["devState"] = String(p.dev_state);
        if (p.weight_valid)
            o["weight"] = fromTenths(p.weight_tenths);
        else
            o["weight"] = nullptr;
        o["selectProfile"] = p.select_profile;
        if (p.phase[0] != '\0')
            o["phase"] = String(p.phase);
        if (p.has_target) {
            o["target"] = fromTenths(p.target_tenths);
            o["mode"] = String(p.mode);
        }
        if (p.has_lead_in)
            o["leadInMs"] = p.lead_in_ms;
        if (p.blocked[0] != '\0')
            o["blocked"] = String(p.blocked);
    }
    JsonArray profiles = out["profiles"].to<JsonArray>();
    for (int i = 0; i < client->profile_count(); i++) {
        const glinkc::ProfileEntry &e = client->profiles()[i];
        JsonObject o = profiles.add<JsonObject>();
        o["index"] = e.index;
        o["name"] = String(e.name);
        o["dose"] = fromTenths(e.dose_tenths);
        if (e.has_grind_size)
            o["grindSize"] = fromTenths(e.grind_size_tenths);
        if (e.grind_unit[0] != '\0')
            o["grindUnit"] = String(e.grind_unit);
        o["writable"] = e.writable;
    }
    if (client->has_result())
        putResult(out["lastResult"].to<JsonObject>(), client->last_result());
}

void GLinkPlugin::publishState(uint32_t now) {
    if (!dirty || now - lastEvent < EVENT_MIN_INTERVAL_MS)
        return;
    dirty = false;
    lastEvent = now;
    if (ws == nullptr || !ws->hasClients())
        return;
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:grinderlink:state";
    fillStatus(doc.as<JsonObject>());
    ws->broadcastJson(doc);
}

#endif // GAGGIMATE_SIM
