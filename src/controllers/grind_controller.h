#pragma once
#include "../config/constants.h"
#include "../hardware/WeightSensor.h"
#include "../hardware/grinder.h"
#include "../logging/grind_logging.h"
#include "grind_mode.h"
#include "grind_start_check.h"
#include "grind_safety.h"
#include "grind_session.h"
#include "grind_strategy.h"
#include "weight_grind_strategy.h"
#include "time_grind_strategy.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>

class DiagnosticsController;

// Forward declaration to avoid circular dependency
struct GrindEventData;


// Flash operation request structure for Core 0 → Core 1 communication
struct FlashOpRequest {
    enum Type {
        START_GRIND_SESSION,
        END_GRIND_SESSION
    };
    
    Type operation_type;
    GrindSessionDescriptor descriptor; // For START_GRIND_SESSION
    char result_string[32];  // "COMPLETE", "TIMEOUT", "OVERSHOOT", etc. (for END_GRIND_SESSION)
    float start_weight;      // For START_GRIND_SESSION (pre-tare snapshot)
    float final_weight;      // For END_GRIND_SESSION
    uint8_t pulse_count;     // For END_GRIND_SESSION
};

// Log message structure for Core 0 → Core 1 communication
struct LogMessage {
    char message[128];  // Pre-formatted log message
};

// Calculated values for a single update cycle - passed to methods to avoid redundant calculations
struct GrindLoopData {
    float current_weight;       // For control logic (low_latency)
    float display_weight;       // For UI display (always calculated)
    uint32_t timestamp_ms;
    float weight_delta;
    float flow_rate;
    uint8_t motor_is_on;
    uint8_t phase_id;
    unsigned long now;
};

// Grind controller state machine phases
enum class GrindPhase {
    IDLE,               // Not grinding
    INITIALIZING,       // Pre-initialization - emit UI event and prepare for grind
    SETUP,              // Initialization - file system operations, logger setup
    TARING,             // Performing tare operation
    TARE_CONFIRM,       // Confirming tare completed
    PREDICTIVE,         // Main grinding with flow prediction
    PULSE_DECISION,     // Deciding if pulse correction needed
    PULSE_EXECUTE,      // Executing precision pulse
    PULSE_SETTLING,     // Waiting for weight to settle after pulse
    FINAL_SETTLING,     // Waiting for weight to settle
    TIME_GRINDING,      // Time-based grinding phase
    TIME_ADDITIONAL_PULSE, // Additional pulse in time mode after completion
    COMPLETED,          // Grind completed (success, overshoot, or max pulses)
    TIMEOUT,            // Grind timed out
    PRIME,              // Optional chute priming/purging grind
    PRIME_SETTLING,     // Settling after priming grind
    PURGE_CONFIRM       // Waiting for user to confirm purge completion
};


struct PulseReport {
    float start_weight;
    float end_weight;
    float duration_ms;
};



// Controls the grinding process with predictive weight stopping and precision pulse corrections
class GrindController {
private:
    friend class WeightGrindStrategy;
    friend class TimeGrindStrategy;

    WeightSensor* weight_sensor;
    Grinder* grinder;
    Preferences* preferences;
    float target_weight;
    uint32_t target_time_ms;
    GrindPhase phase;
    unsigned long start_time;
    unsigned long phase_start_time;
    unsigned long time_grind_start_ms;
    
    float tolerance;
    GrindMode mode;
    GrinderPurgeMode grinder_purge_mode_for_session;
    float grinder_purge_amount_g_for_session;

    // Timeout tracking
    GrindPhase timeout_phase;   // Phase when timeout occurred
    unsigned long timeout_pause_start;  // When we entered a paused state (PURGE_CONFIRM)
    unsigned long timeout_offset_ms;    // Accumulated time in paused states to exclude from timeout
    
    int pulse_attempts;
    unsigned long pulse_start_time;
    float current_pulse_duration_ms;
    
    
    
    float predictive_end_weight;
    volatile float grind_latency_ms;        // Thread-safe for Core 0 access
    PulseReport pulse_history[GRIND_MAX_PULSE_ATTEMPTS];
    volatile float motor_stop_target_weight; // Thread-safe for Core 0 access
    float final_weight; // Stores the final settled weight from final_measurement()

    // Flow detection confirmation variables
    bool flow_start_confirmed;
    // Dynamic pulse algorithm variables
    volatile float pulse_flow_rate;    // Thread-safe for Core 0 access
    
    // Loop counter variables for performance tracking
    volatile uint16_t current_phase_loop_count;  // Thread-safe for Core 0 access

    // Logging support
    uint8_t current_profile_id;
    GrindEvent event_in_progress; // Used to build data for the current phase event
    
    // State tracking for measurement calculations (eliminates calculations in logger)
    float last_logged_weight;       // Previous weight for delta calculation
    unsigned long last_logged_time; // Previous timestamp for relative timing
    bool force_measurement_log;     // Flag to force measurement logging on next update cycle

    // UI event system - thread-safe Core 0 → Core 1 communication
    QueueHandle_t ui_event_queue;
    
    bool control_loop_paused_;      // Indicates control loop is suspended (e.g., purge confirmation)
    
    // Flash operation queue - thread-safe Core 0 → Core 1 communication
    QueueHandle_t flash_op_queue;
    static const int FLASH_OP_QUEUE_SIZE = 5;
    
    // Log message queue - thread-safe Core 0 → Core 1 communication
    QueueHandle_t log_queue;
    static const int LOG_QUEUE_SIZE = 20;
    
    // Time mode pulse tracking
    int additional_pulse_count;
    uint32_t pulse_duration_ms;
    
    void (*ui_event_callback)(const GrindEventData&) = nullptr;
    bool ui_ready_for_setup = false; // Flag to track UI acknowledgment of INITIALIZING phase
    
    // Flag to prevent repeated flash operations for terminal phases (COMPLETED/TIMEOUT)
    bool session_end_flash_queued = false;
    char last_error_message[32];

    GrindSessionDescriptor session_descriptor;
    GrindStrategyContext strategy_context;
    IGrindStrategy* active_strategy = nullptr;
    WeightGrindStrategy weight_strategy;
    TimeGrindStrategy time_strategy;

    // Mechanical instability tracking
    int mechanical_anomaly_count_ = 0;
    unsigned long last_mechanical_event_ms_ = 0;
    float last_mechanical_weight_ = 0.0f;
    bool mechanical_monitor_initialized_ = false;

    DiagnosticsController* diagnostics_controller_ = nullptr;

    // Safety monitoring (weight mode)
    // Same values as GRIND_SAFETY_REASON_* in grind_safety.h (the pure abort-reason mapping); grind_controller.cpp
    // static_asserts it
    static constexpr uint8_t SAFETY_REASON_SENSOR = 1 << 0;
    static constexpr uint8_t SAFETY_REASON_TARE = 1 << 1;
    static constexpr uint8_t SAFETY_REASON_PRIME_FLOW = 1 << 2;
    static constexpr uint8_t SAFETY_REASON_PREDICTIVE_FLOW = 1 << 3;
    static constexpr uint8_t SAFETY_REASON_STALL = 1 << 4;
    uint32_t max_sample_age_ms_ = 0;          // Oldest newest-sample age seen during this grind (session log)
    uint8_t safety_logged_mask_ = 0;          // Log-only mode: each reason is reported once per grind
    unsigned long tare_settle_start_ms_ = 0;  // When the tare finished in TARE_CONFIRM (0 = not yet)
    float predictive_min_weight_ = 0.0f;      // Lowest weight seen since PREDICTIVE entry (gain baseline)
    float predictive_peak_gain_g_ = 0.0f;     // Highest gain over that baseline in PREDICTIVE
    GrindStallTracker stall_tracker_ = {};    // Mid-grind stall check: highest weight and when it last rose (PREDICTIVE)

    // Motor response latency - runtime configurable
    float motor_response_latency_ms;

    // Grind freshness tracking
    bool grinder_purged_since_boot;      // Tracks if grinder has been used since boot (RAM only)
    uint64_t last_purge_runtime_ms;      // Runtime when last grind completed (persisted)

public:
    enum class GrindSessionResult {
        UNKNOWN,
        SUCCESS,
        OVERSHOOT,
        MAX_PULSES,
        TIMEOUT,
        ERROR
    };

    // Authoritative record of one finished grind (a normal end, an abort or a stop). A session keeps its first result,
    // except that a real outcome replaces a stop that raced it (grind_result_may_replace()); a skipped publish does not
    // move result_count(). Consumer rules (GLink):
    //  - key on a change of result_count() plus session_id; retry on the next tick while read_last_result() is false;
    //  - the phase becomes COMPLETED/TIMEOUT slightly before the result is published (same control cycle);
    //  - a stop in the ~20 ms before the phase leaves IDLE publishes nothing: never wait forever for a result;
    //  - a GLink start forces WEIGHT mode for its session; the consumer must restore UIManager::current_mode and the
    //    grinding screen's mode when the screen returns to READY (STOPPED/COMPLETED/TIMEOUT events).
    // A stop (stop_grind()) is reported as outcome UNKNOWN with stopped = true, never as ERROR. An ERROR outcome
    // always has an abort_reason other than NONE.
    struct GrindResult {
        uint32_t session_id = 0;          // 1-based count of grinds since boot (RAM only; 0 = none yet)
        GrindSessionResult outcome = GrindSessionResult::UNKNOWN;
        GrindAbortReason abort_reason = GrindAbortReason::NONE;
        float final_weight_g = 0.0f;      // settled weight for SUCCESS/OVERSHOOT/MAX_PULSES, else the last live reading
        uint8_t pulses = 0;               // pulse corrections (saturates at 255)
        uint32_t total_time_ms = 0;       // start to end, including a purge-confirm wait
        uint32_t motor_time_ms = 0;       // motor-on time seen at control-cycle resolution
        bool motor_ran = false;           // the motor was seen running at least once (coffee may be in the cup)
        bool stopped = false;             // ended by stop_grind() (touch, web or BLE stop; never a timeout)
    };

private:
    GrindSessionResult last_session_result_ = GrindSessionResult::UNKNOWN;
    GrindAbortReason abort_reason_ = GrindAbortReason::NONE;  // set with every ERROR result (control task only)
    std::atomic<bool> stop_requested_{false};  // set by request_stop() from any task, consumed by update()

    // Result slot: a seqlock. Writers (control task at COMPLETED/TIMEOUT, any task in stop_grind()) make result_seq_
    // odd, store the fields (relaxed) and make it even again with a release store; the count of finished publishes is
    // result_seq_ / 2. Readers (UI task) load result_seq_ with acquire, read the fields (relaxed) and accept them only
    // if result_seq_ is unchanged and even after an acquire fence. Not volatile: every shared field is a std::atomic.
    struct ResultSlot {
        std::atomic<uint32_t> session_id{0};
        std::atomic<uint32_t> total_time_ms{0};
        std::atomic<uint32_t> motor_time_ms{0};
        std::atomic<uint32_t> final_weight_bits{0};  // float bit pattern
        std::atomic<uint8_t> outcome{0};
        std::atomic<uint8_t> abort_reason{0};
        std::atomic<uint8_t> pulses{0};
        std::atomic<bool> motor_ran{false};
        std::atomic<bool> stopped{false};
    };
    ResultSlot result_slot_;
    std::atomic<uint32_t> result_seq_{0};
    std::atomic<uint32_t> session_counter_{0};   // incremented by start_grind() (UI task), copied into the result
    // Motor-on time of the running grind; written by the control task, read by stop_grind() on the UI task
    std::atomic<uint32_t> motor_time_ms_{0};
    std::atomic<bool> motor_ran_{false};
    bool motor_sample_on_ = false;               // control task only
    unsigned long motor_sample_ms_ = 0;          // control task only

public:
    void init(WeightSensor* lc, Grinder* gr, Preferences* prefs);
    // Why a grind cannot start right now (NONE = it can). Cheap and side-effect free; safe to poll.
    GrindStartBlock start_block() const;
    // Starts a grind, or refuses (and logs why) and returns the reason. NONE = started.
    GrindStartBlock start_grind(float target_weight, uint32_t target_time_ms, GrindMode grind_mode);
    void user_tare_request();
    void return_to_idle(); // Called by UI to acknowledge completion/timeout
    void stop_grind();
    // Any task: asks the control task to stop a running grind (same effect as stop_grind()). Consumed at the top of
    // update() within one control cycle; ignored if no grind is running by then. Used by the web panel's stop.
    void request_stop() { stop_requested_.store(true); }
    void continue_from_purge(); // Called by UI to continue from PURGE_CONFIRM to PREDICTIVE
    void update(); // Core 0 main control method - runs at fixed RTOS interval
    
    // Time mode pulse functionality
    void start_additional_pulse(); // Start an additional 100ms pulse in time mode
    bool can_pulse() const; // Check if additional pulses are allowed
    int get_additional_pulse_count() const { return additional_pulse_count; }
    
    // UI event system
    void set_ui_event_callback(void (*callback)(const GrindEventData&));
    GrindSessionResult get_last_session_result() const { return last_session_result_; }
    // Id of the newest grind session started (1-based since boot, 0 before the first). Any task.
    uint32_t session_id() const { return session_counter_.load(std::memory_order_relaxed); }
    // Number of grind results published since boot (a stop counts). Acquire load: when it differs from the value seen
    // before, read_last_result() returns that result or a newer one. Defined while idle (0 before the first grind).
    uint32_t result_count() const { return result_seq_.load(std::memory_order_acquire) / 2; }
    // Copies the newest published result. False (and *out untouched) before the first grind or if a writer kept the
    // slot busy for every retry; try again on the next UI tick. Any task; never blocks.
    bool read_last_result(GrindResult* out) const;
    void ui_acknowledge_phase_transition(); // Called by UI to confirm phase transition
    void process_queued_ui_events(); // Core 1: Process events from Core 0 queue
    QueueHandle_t get_ui_event_queue() const { return ui_event_queue; }
    
    // Flash operation system
    void process_queued_flash_operations(); // Core 1: Process flash ops from Core 0 queue
    void queue_flash_operation(const FlashOpRequest& request); // Core 0: Queue flash operation
    
    // Log message system
    void process_queued_log_messages(); // Core 1: Process log messages from Core 0 queue
    void queue_log_message(const char* format, ...); // Core 0: Queue formatted log message
    
    bool is_active() const;
    bool is_grind_running() const; // A grind may still drive the motor (not IDLE/COMPLETED/TIMEOUT)
    int phase_value() const { return static_cast<int>(phase); }  // GrindPhase as a number (web panel status)
    bool is_control_loop_paused() const { return control_loop_paused_; }
    float get_target_weight() const { return target_weight; }
    uint32_t get_target_time_ms() const { return target_time_ms; }
    static constexpr const char* PREF_KEY_PRIME_ENABLED = "prime_enabled";
    static constexpr const char* PREF_KEY_GRINDER_MODE = "grinder_mode";
    static constexpr const char* PREF_KEY_GRINDER_AMOUNT_G = "grinder_amt_g";
    static constexpr const char* PREF_KEY_GRIND_FRESHNESS_HOURS = "freshness_hrs";
    static constexpr const char* PREF_KEY_LAST_GRIND_RUNTIME = "last_grind_ms";
    GrindMode get_mode() const { return mode; }
    const GrindSessionDescriptor& get_session_descriptor() const { return session_descriptor; }
    
    // Grind logging functions
    void set_grind_profile_id(uint8_t profile_id) { current_profile_id = profile_id; session_descriptor.profile_id = profile_id; }
    void send_measurements_data();           // Send structured measurement data via serial
    
    // Getter methods for logger (to eliminate calculations in logger)
    float get_current_flow_rate() const;
    float get_motor_stop_target_weight() const { return motor_stop_target_weight; }
    float get_grind_latency_ms() const { return grind_latency_ms; }
    float get_last_logged_weight() const { return last_logged_weight; }
    void set_last_logged_weight(float weight) { last_logged_weight = weight; } // Thread-safe setter

    int get_mechanical_anomaly_count() const { return mechanical_anomaly_count_; }
    void reset_mechanical_anomaly_count();

    void set_diagnostics_controller(DiagnosticsController* diagnostics) { diagnostics_controller_ = diagnostics; }

    // Motor response latency accessors
    float get_motor_response_latency() const { return motor_response_latency_ms; }
    void set_motor_response_latency(float value);
    float get_min_pulse_duration() const { return motor_response_latency_ms; }
    float get_max_pulse_duration() const { return motor_response_latency_ms + GRIND_MOTOR_MAX_PULSE_DURATION_MS; }
    void load_motor_latency();
    void save_motor_latency(float value);

    // Grind freshness accessors
    bool get_grinder_purged_since_boot() const { return grinder_purged_since_boot; }
    uint64_t get_last_purge_runtime_ms() const { return last_purge_runtime_ms; }
    
    // Removed - predictive logic now inline in update_realtime()
    
    
private:
    void switch_phase(GrindPhase new_phase, const GrindLoopData& loop_data = {});
    void final_measurement(const GrindLoopData& loop_data);
    void monitor_mechanical_instability(const GrindLoopData& loop_data);
    void abort_grind(GrindSessionResult result, GrindAbortReason reason, const char* ui_text, const char* log_text,
                     const GrindLoopData& loop_data);
    void track_motor_time(const GrindLoopData& loop_data);
    // Publishes the end of the session (see GrindResult). weight_g = final or last live weight.
    void publish_result(GrindSessionResult outcome, GrindAbortReason reason, float weight_g, bool stopped);

    bool check_safety(const GrindLoopData& loop_data);  // True when the grind was aborted this cycle
    bool raise_safety(uint8_t reason_bit, const char* ui_text, const char* log_text,
                      const char* detail, const GrindLoopData& loop_data);  // Abort, or log once when aborts are disabled
    bool check_timeout() const;
    uint8_t get_current_phase_id() const;
    
    // UI event emission - thread-safe for Core 0
    void emit_ui_event(const GrindEventData& data);
    void emit_progress_update(const GrindLoopData& loop_data);
    
    // Core 0 control methods  
    bool should_log_measurements() const;
    
    // Internal state methods (moved from public to prevent polling)
    bool show_taring_text() const { return phase == GrindPhase::INITIALIZING || phase == GrindPhase::SETUP || phase == GrindPhase::TARING || phase == GrindPhase::TARE_CONFIRM; }
    bool is_completed() const { return phase == GrindPhase::COMPLETED; }
    bool is_timeout() const { return phase == GrindPhase::TIMEOUT; } 
    int get_progress_percent() const;
    float get_grind_time() const;
    GrindPhase get_phase() const { return phase; }
    GrindPhase get_timeout_phase() const { return timeout_phase; }
    const char* get_phase_name(GrindPhase p = static_cast<GrindPhase>(-1)) const;

    void set_error_message(const char* message);
};
