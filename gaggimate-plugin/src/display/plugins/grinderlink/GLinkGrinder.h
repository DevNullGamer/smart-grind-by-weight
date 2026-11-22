#ifndef GAGGIMATE_SIM
#pragma once

#include "GLinkGrinderCore.h"
#include "RxRing.h"
#include <BluetoothDeviceRegistry.h>
#include <BluetoothGrinder.h>
#include <atomic>
#include <mutex>

// GLink grinder driver (chg025): a Smart Grind By Weight grinder as a BluetoothGrinder of GaggiMate's BluetoothDevices
// framework (spec 3.2). Device glue only: the session logic is glinkc::GrinderCore (pure, host-tested). Device code, so the
// whole file is empty in the simulator build.
//
// Threads: the notification callback (NimBLE host task) only pushes into the byte ring under rxMutex_ (no log, no
// allocation). connect(), update(), disconnect() and applySettings() all run on the loop task. The framework creates one
// instance per connect attempt, so the constructor allocates nothing; connect() allocates the ring and the core with
// new (std::nothrow) and disconnect() or the destructor frees them.
//
// Where start() and stop() would go: none exists. The framework's BluetoothGrinder has no start/stop interface yet (left to
// its maintainer); glinkc::Client::start/stop stay unused until it has. shutdown() is deliberately not overridden:
// GaggiMate entering standby must never power the grinder off.
class GLinkGrinder : public BluetoothGrinder, private glinkc::Sink {
  public:
    // Registered with the framework under this id (spec 3.2); handles() matches the advertised name prefix.
    static constexpr const char *DRIVER_ID = "plugin-glink-grinder";
    static void registerDriver();
    static bool handles(const DiscoveredDevice &device);

    // connect() refuses while the probe says GaggiMate is busy (a brew or grind runs, or the system is not ready): the connect
    // blocks the loop task for seconds. No probe installed counts as busy.
    using BusyProbe = bool (*)();
    static void setBusyProbe(BusyProbe probe);

    explicit GLinkGrinder(const DiscoveredDevice &device);
    // Ends the link before the members the notification callback uses are destroyed.
    ~GLinkGrinder() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() override;
    void update() override;

    // Queued as the wanted Profile Dose settings (dose, size and rpm; feedRpm is ignored); pushed from update() once the
    // link is ready.
    bool applySettings(const GrinderSettings &settings) override;
    bool supportsGrindSize() const override { return true; }
    bool supportsDose() const override { return true; }
    // True only on a ready link to a grinder that advertises protoMinor 2 or more (older firmware refuses an rpm).
    bool supportsRpm() const override { return core_ != nullptr && core_->rpm_supported(); }

  private:
    static constexpr size_t RX_RING_BYTES = 8192; // about 370 packets of 20 bytes (spec 3.7)

    // glinkc::Sink
    bool write_packet(const uint8_t *packet, size_t len) override;

    bool discoverCharacteristics();
    bool subscribe();
    void drainRing(uint32_t now);
    void publishToFramework();
    void logProgress(uint32_t now);
    void dropLink(const char *reason);
    void releaseLink();
    void logLostReason();
    void logTxPower();
    void pushWanted();

    NimBLERemoteCharacteristic *rx_ = nullptr;
    NimBLERemoteCharacteristic *tx_ = nullptr;

    // Allocated in connect(), freed in disconnect() and the destructor.
    uint8_t *ringStorage_ = nullptr;
    glinkc::GrinderCore *core_ = nullptr;

    // Shared with the NimBLE host task (the notification callback).
    std::mutex rxMutex_;
    glinkc::RxRing ring_;
    std::atomic<bool> rxOverflow_{false};

    // Loop task only.
    bool readyLogged_ = false;
    uint32_t heapBefore_ = 0;
    uint32_t rssiDue_ = 0;
    glinkc::SyncStage lastStage_ = glinkc::SyncStage::IDLE;
    bool haveWanted_ = false; // applySettings() was called: the wanted values below are valid
    bool wantedHasDose_ = false;
    int32_t wantedDoseTenths_ = 0;
    bool wantedHasSize_ = false;
    int32_t wantedSizeTenths_ = 0;
    bool wantedHasRpm_ = false;
    int32_t wantedRpm_ = 0;
};

#endif // GAGGIMATE_SIM
