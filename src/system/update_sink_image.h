#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "image_header.h"
#include "sha_hex.h"

enum class SinkError : uint8_t {
    OK,
    BAD_ARGUMENT,     // null pointer / bad sha256 text
    ALREADY_ACTIVE,   // begin() while a transfer is open
    NOT_ACTIVE,       // write()/finish() without a successful begin()
    NO_PARTITION,     // no OTA slot to write to
    BAD_SIZE,         // announced size outside [min, slot size] (see header_result())
    BEGIN_FAILED,     // esp_ota_begin failed (flash erase / partition)
    BAD_HEADER,       // first bytes are not a valid ESP32-S3 app image (see header_result())
    TOO_MUCH_DATA,    // more bytes than announced
    WRITE_FAILED,     // esp_ota_write failed
    HASH_FAILED,      // SHA-256 engine error
    SIZE_MISMATCH,    // finish() with fewer bytes than announced
    SHA_MISMATCH,     // SHA-256 differs from the announced one
    IMAGE_INVALID,    // esp_ota_end rejected the image (checksum / structure)
    BOOT_SET_FAILED   // esp_ota_set_boot_partition failed
};

/**
 * ImageSink - writes a raw, complete firmware image into the next OTA slot.
 *
 * begin() -> write()* -> finish(); abort() at any time. Every failure aborts the transfer (the slot is left
 * invalid, the running image is untouched) and is reported through last_error()/reason(). Nothing reaches
 * flash before the first UPDATE_IMAGE_HEADER_BYTES bytes passed the header check. The sink never restarts
 * the device and does not take the UpdateGuard; the caller does both. Not thread safe: the caller serialises
 * calls (one HTTP request at a time).
 */
class ImageSink {
public:
    ImageSink() = default;
    ~ImageSink() { abort(); }
    ImageSink(const ImageSink&) = delete;
    ImageSink& operator=(const ImageSink&) = delete;

    // total_size: announced image bytes; expected_sha256_hex: 64 hex characters of the whole image.
    SinkError begin(uint32_t total_size, const char* expected_sha256_hex);
    // Appends the next chunk of the image (any chunk size, including 0).
    SinkError write(const uint8_t* data, size_t len);
    // Verifies size and SHA-256, validates the image and makes the new slot the boot partition.
    // The caller restarts the device afterwards.
    SinkError finish();
    // Drops the transfer and resets all state; safe to call at any time, any number of times.
    void abort();

    bool active() const { return active_; }
    uint32_t received() const { return received_; }
    uint32_t total() const { return total_; }
    SinkError last_error() const { return error_; }
    // Only meaningful for BAD_HEADER / BAD_SIZE
    ImageHeaderResult header_result() const { return header_result_; }
    // Short text for last_error(), suitable for the UI / HTTP response
    const char* reason() const;
    // Label of the slot being written (empty when idle)
    const char* partition_label() const { return partition_ ? partition_->label : ""; }

private:
    bool active_ = false;
    bool ota_open_ = false;        // esp_ota_begin succeeded and esp_ota_end was not called yet
    bool header_checked_ = false;
    esp_ota_handle_t handle_ = 0;
    const esp_partition_t* partition_ = nullptr;
    uint32_t total_ = 0;
    uint32_t received_ = 0;
    uint8_t head_[UPDATE_IMAGE_HEADER_BYTES];
    size_t head_len_ = 0;
    uint8_t expected_sha_[SHA256_DIGEST_LEN];
    mbedtls_sha256_context sha_ctx_;
    bool sha_ready_ = false;
    SinkError error_ = SinkError::OK;
    ImageHeaderResult header_result_ = ImageHeaderResult::OK;

    SinkError fail(SinkError error);
    SinkError write_through(const uint8_t* data, size_t len);
    void reset_state();
};
