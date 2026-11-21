#ifndef GAGGIMATE_SIM

#include "GLinkGrinder.h"

#include "bluetooth/glink_disconnect.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <nimble/nimble/host/include/host/ble_hs.h>
#include <new>

// The display is a NimBLE central of the controller and of a scale; the grinder is the third link.
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 3, "GLinkGrinder needs CONFIG_BT_NIMBLE_MAX_CONNECTIONS of at least 3");

namespace {
const NimBLEUUID serviceUUID(GLINK_SERVICE_UUID);
const NimBLEUUID rxCharacteristicUUID(GLINK_RX_CHAR_UUID);
const NimBLEUUID txCharacteristicUUID(GLINK_TX_CHAR_UUID);

constexpr uint32_t COOLDOWN_MS = 10000; // no connect this soon after a drop the driver itself caused (spec 3.2)
constexpr uint32_t RSSI_LOG_MS = 5000;  // the link's RSSI is logged this often while linked
constexpr int NO_REASON = -1;
constexpr int32_t TENTHS_CAP = 100000;  // anything above is out of range for the core anyway; keeps lroundf safe

bool (*busyProbe)() = nullptr;

// Loop task only (each connect attempt is a new instance, so these outlive it).
bool cooldownActive = false;
uint32_t cooldownStart = 0;
bool resourcesLogged = false; // loop stack high-water mark and free heap are logged once, at the first ready link

// Disconnect reason of the link (spec 3.2): the NimBLE client callback does not carry it, so the GAP listener keeps it.
// Runs on the NimBLE host task: an atomic store, no logging, no allocation. The peer address is written before the connect.
ble_gap_event_listener gapListener; // the stack keeps the pointer: lives for the whole run
uint8_t peerAddress[6];
std::atomic<bool> peerKnown{false};
std::atomic<int> downReason{NO_REASON};

int onGapEvent(struct ble_gap_event *event, void *) {
    if (event->type != BLE_GAP_EVENT_DISCONNECT || !peerKnown.load()) {
        return 0;
    }
    if (memcmp(event->disconnect.conn.peer_id_addr.val, peerAddress, sizeof(peerAddress)) == 0) {
        downReason.store(event->disconnect.reason);
    }
    return 0;
}

int32_t tenthsOf(float value) {
    if (!std::isfinite(value) || value > static_cast<float>(TENTHS_CAP) / 10.0f) {
        return TENTHS_CAP;
    }
    return static_cast<int32_t>(lroundf(value * 10.0f));
}

GrinderState frameworkState(glinkc::DevClass devClass) {
    switch (devClass) {
    case glinkc::DevClass::IDLE:
        return GrinderState::IDLE;
    case glinkc::DevClass::GRINDING:
        return GrinderState::GRINDING;
    case glinkc::DevClass::BUSY:
        return GrinderState::BUSY;
    case glinkc::DevClass::UNKNOWN:
    default:
        return GrinderState::UNKNOWN;
    }
}
} // namespace

void GLinkGrinder::registerDriver() {
    BluetoothDeviceRegistry::getInstance()->registerDriver<GLinkGrinder>(DRIVER_ID, &GLinkGrinder::handles);
}

// The grinder advertises its name "GrindByWeight" (BLE_DEVICE_NAME); a name with a suffix still matches.
bool GLinkGrinder::handles(const DiscoveredDevice &device) { return device.getName().rfind(BLE_DEVICE_NAME, 0) == 0; }

void GLinkGrinder::setBusyProbe(BusyProbe probe) { busyProbe = probe; }

GLinkGrinder::GLinkGrinder(const DiscoveredDevice &device) : BluetoothGrinder(device) {}

GLinkGrinder::~GLinkGrinder() {
    clientCleanup();
    releaseLink();
}

//------------------------------------------------------------------------------
// Connect
//------------------------------------------------------------------------------

bool GLinkGrinder::connect() {
    if (clientIsConnected()) {
        log("Already connected\n");
        return true;
    }
    // Refuse before any radio use: a connect blocks the loop task for seconds (spec 3.2, F7). The framework rescans and
    // retries when the grinder is seen again.
    if (busyProbe == nullptr || busyProbe()) {
        log("Not connecting: GaggiMate is busy\n");
        return false;
    }
    if (NimBLEDevice::getClientListSize() >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        log("Not connecting: no free Bluetooth connection (controller, scale and grinder use all three)\n");
        return false;
    }
    const uint32_t now = millis();
    if (cooldownActive && now - cooldownStart < COOLDOWN_MS) {
        log("Not connecting: cooling down after a dropped session\n");
        return false;
    }
    cooldownActive = false;

    log("Connecting to %s[%s]\n", getDeviceName().c_str(), getDeviceAddress().c_str());
    releaseLink();
    heapBefore_ = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // One block: the byte ring, then the scratch packet drainRing() pops into (off the loop task's stack).
    ringStorage_ = new (std::nothrow) uint8_t[RX_RING_BYTES + GLINK_MAX_PACKET_BYTES];
    core_ = new (std::nothrow) glinkc::GrinderCore(*this);
    if (ringStorage_ == nullptr || core_ == nullptr) {
        log("Out of memory for the link buffers, not connecting\n");
        releaseLink();
        return false;
    }
    ring_.init(ringStorage_, RX_RING_BYTES);
    rxOverflow_ = false;
    readyLogged_ = false;
    lastStage_ = glinkc::SyncStage::IDLE;
    pushWanted();

    const int listenerRc = ble_gap_event_listener_register(&gapListener, &onGapEvent, nullptr);
    if (listenerRc != 0 && listenerRc != BLE_HS_EALREADY) { // EALREADY: registered by an earlier connect
        log("Disconnect reasons will not be logged (listener rc %d)\n", listenerRc);
    }
    peerKnown = false;
    memcpy(peerAddress, getDevice().getAddress().getNative(), sizeof(peerAddress));
    peerKnown = true;
    downReason = NO_REASON;

    if (!clientConnect()) {
        log("Could not connect to the grinder\n");
        logLostReason();
        clientCleanup();
        releaseLink();
        return false;
    }
    const char *failure = nullptr;
    if (!discoverCharacteristics()) {
        failure = "The device has no GLink service (old grinder firmware or another device?)";
    } else if (!subscribe()) {
        failure = "Could not subscribe to the grinder";
    }
    if (failure != nullptr) {
        log("%s\n", failure);
        logLostReason();
        cooldownActive = true; // a device that fails the handshake must not be retried at once
        cooldownStart = millis();
        clientCleanup();
        rx_ = nullptr;
        tx_ = nullptr;
        releaseLink();
        return false;
    }

    const uint16_t mtu = clientGetMTU();
    const size_t packet = mtu > 23 ? std::min<size_t>(mtu - 3, GLINK_MAX_PACKET_BYTES) : GLINK_DEFAULT_PACKET_BYTES;
    logTxPower();
    rssiDue_ = millis() + RSSI_LOG_MS;
    // The hello goes out inside connect(): the grinder drops a client that stays silent for 10 s (spec 2).
    if (!core_->on_connected(packet, millis())) {
        log("The hello could not be written\n");
        cooldownActive = true;
        cooldownStart = millis();
        clientCleanup();
        rx_ = nullptr;
        tx_ = nullptr;
        releaseLink();
        return false;
    }
    log("Linked, packet size %u, hello sent\n", static_cast<unsigned>(packet));
    return true;
}

bool GLinkGrinder::discoverCharacteristics() {
    NimBLERemoteService *service = clientGetService(serviceUUID);
    if (service == nullptr) {
        return false;
    }
    rx_ = service->getCharacteristic(rxCharacteristicUUID);
    tx_ = service->getCharacteristic(txCharacteristicUUID);
    return rx_ != nullptr && tx_ != nullptr && rx_->canWrite() && tx_->canNotify();
}

// Subscribe before the hello: its answer is a notification. The callback runs on the NimBLE host task: it only pushes into
// the ring (no log, no allocation); a packet that cannot be a GLink packet or does not fit sets the overflow flag.
bool GLinkGrinder::subscribe() {
    auto callback = [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
        if (length == 0) {
            return;
        }
        if (length > GLINK_MAX_PACKET_BYTES) {
            rxOverflow_ = true;
            return;
        }
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (!ring_.push(data, length)) {
            rxOverflow_ = true;
        }
    };
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        ring_.clear();
    }
    return tx_->subscribe(true, callback, true);
}

bool GLinkGrinder::write_packet(const uint8_t *packet, size_t len) {
    if (rx_ == nullptr || !clientIsConnected()) {
        return false;
    }
    return rx_->writeValue(packet, len, true);
}

// NimBLEDevice::getPower() returns -128 for levels outside its 8-level table: ask the controller and decode it.
void GLinkGrinder::logTxPower() {
    int dbm = 0;
    if (glink_tx_power_level_dbm(static_cast<int>(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)), &dbm)) {
        char text[16];
        glink_format_dbm(text, sizeof(text), dbm);
        log("TX power %s\n", text);
    } else {
        log("TX power unknown\n");
    }
}

//------------------------------------------------------------------------------
// Disconnect and cleanup
//------------------------------------------------------------------------------

void GLinkGrinder::disconnect() {
    const bool hadLink = core_ != nullptr;
    clientCleanup(); // ends the link first: the callback must not run against freed members
    rx_ = nullptr;
    tx_ = nullptr;
    if (hadLink) {
        logLostReason();
    }
    releaseLink();
    setState(GrinderState::UNKNOWN);
    setReportedSettings(GrinderSettings());
}

bool GLinkGrinder::isConnected() { return clientIsConnected(); }

void GLinkGrinder::releaseLink() {
    delete core_;
    core_ = nullptr;
    delete[] ringStorage_;
    ringStorage_ = nullptr;
}

// The reason the stack reported for the link that was just lost, or for a failed connect.
void GLinkGrinder::logLostReason() {
    const int reason = downReason.exchange(NO_REASON);
    if (reason == NO_REASON) {
        log("Link lost, the stack reported no reason\n");
        return;
    }
    char text[48];
    glink_disconnect_describe(text, sizeof(text), reason);
    log("Link lost, %s\n", text);
}

// The driver ends a session it cannot continue (protocol error, ring overflow); the framework drops the device on its
// next tick (isConnected() is false) and calls disconnect().
void GLinkGrinder::dropLink(const char *reason) {
    log("Dropping the link: %s\n", reason);
    cooldownActive = true;
    cooldownStart = millis();
    clientCleanup();
    rx_ = nullptr;
    tx_ = nullptr;
}

//------------------------------------------------------------------------------
// Settings
//------------------------------------------------------------------------------

// Dose and grind size of the profile GaggiMate selected, as the wanted Profile Dose (queued, like the Motto driver).
bool GLinkGrinder::applySettings(const GrinderSettings &settings) {
    haveWanted_ = true;
    wantedHasDose_ = std::isfinite(settings.dose) && settings.dose > 0.0f;
    wantedDoseTenths_ = wantedHasDose_ ? tenthsOf(settings.dose) : 0;
    wantedHasSize_ = std::isfinite(settings.grindSize) && settings.hasGrindSize();
    wantedSizeTenths_ = wantedHasSize_ ? tenthsOf(settings.grindSize) : 0;
    pushWanted();
    return isConnected();
}

void GLinkGrinder::pushWanted() {
    if (core_ != nullptr && haveWanted_) {
        core_->set_wanted(wantedHasDose_, wantedDoseTenths_, wantedHasSize_, wantedSizeTenths_);
    }
}

//------------------------------------------------------------------------------
// update() (loop task, about once a second)
//------------------------------------------------------------------------------

void GLinkGrinder::update() {
    if (core_ == nullptr || !isConnected()) {
        return;
    }
    const uint32_t now = millis();
    // Drain BEFORE the tick: a loop that stalled (another device's 5 s connect) must not fire the 5 s ready or response
    // timeouts while the answer already sits in the ring.
    drainRing(now);
    if (rxOverflow_.load()) { // a gap in the stream: the reassembler must not see it
        dropLink("notification ring overflow");
        return;
    }
    const char *reason = nullptr;
    if (!core_->must_disconnect(&reason)) {
        core_->update(now);
    }
    publishToFramework();
    if (core_->must_disconnect(&reason)) {
        dropLink(reason);
        return;
    }
    logProgress(now);
}

// Pops every packet into the core. rxMutex_ is held only to pop (never while the core runs).
void GLinkGrinder::drainRing(uint32_t now) {
    uint8_t *packet = ringStorage_ + RX_RING_BYTES;
    const char *reason = nullptr;
    for (;;) {
        if (rxOverflow_.load()) { // checked before every packet: a packet behind the gap must not be fed
            return;
        }
        size_t length = 0;
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            if (ring_.count() == 0) {
                return;
            }
            length = ring_.pop(packet, GLINK_MAX_PACKET_BYTES);
        }
        if (length == 0) {
            continue;
        }
        core_->on_packet(packet, length, now);
        if (core_->must_disconnect(&reason)) {
            return;
        }
    }
}

// State, the reported settings and a finished grind go to the framework. reportGrindFinished() is called here only: on
// the loop task, from update(), never from a NimBLE callback.
void GLinkGrinder::publishToFramework() {
    setState(frameworkState(core_->dev_class()));
    GrinderSettings reported;
    int32_t tenths = 0;
    if (core_->selected_profile_dose(&tenths)) {
        reported.dose = static_cast<float>(tenths) / 10.0f;
    }
    if (core_->selected_profile_size(&tenths)) {
        reported.grindSize = static_cast<float>(tenths) / 10.0f;
    }
    setReportedSettings(reported);
    if (core_->take_finished(&tenths)) {
        log("Grind finished, %d.%d g\n", static_cast<int>(tenths / 10), static_cast<int>(tenths % 10));
        reportGrindFinished(static_cast<float>(tenths) / 10.0f);
    }
}

// Log lines of spec 3.2: data channel ready, push progress, RSSI. Integers only: float printf is stack-hungry and the
// loop task has little headroom.
void GLinkGrinder::logProgress(uint32_t now) {
    if (!readyLogged_ && core_->ready()) {
        readyLogged_ = true;
        const uint32_t heapNow = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        log("Data channel ready, internal heap %u bytes free (%u before the connect)\n", static_cast<unsigned>(heapNow),
            static_cast<unsigned>(heapBefore_));
        if (!resourcesLogged) {
            resourcesLogged = true;
            log("Loop task stack high-water mark: %u bytes free at least, largest internal block %u bytes\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        }
    }
    if (core_->wanted_note() == glinkc::SyncNote::OUT_OF_RANGE_DOSE) {
        log("Profile dose outside 5.0 to 60.0 g, not sent to the grinder\n");
    }
    const glinkc::SyncStage stage = core_->sync_stage();
    if (stage != lastStage_) {
        switch (stage) {
        case glinkc::SyncStage::UPDATE_PENDING:
            log("Profile dose push queued\n");
            break;
        case glinkc::SyncStage::SELECT_PENDING:
            log("Profile dose stored, selecting the profile on the grinder\n");
            break;
        case glinkc::SyncStage::GAVE_UP:
            log("Giving up on the profile dose push (%s)\n", core_->sync_gave_up_code());
            break;
        case glinkc::SyncStage::IDLE:
        default:
            log("Profile dose is in sync with the grinder\n");
            break;
        }
        lastStage_ = stage;
    }
    if (static_cast<int32_t>(now - rssiDue_) >= 0) {
        rssiDue_ = now + RSSI_LOG_MS;
        const int rssi = getRSSI(); // 0: the read failed (a real reading is never 0 dBm)
        if (rssi != 0) {
            log("RSSI %d dBm\n", rssi);
        }
    }
}

#endif // GAGGIMATE_SIM
