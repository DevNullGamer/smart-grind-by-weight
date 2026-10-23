#include "update_sink_image.h"
#include "../config/constants.h"
#include <esp_partition.h>
#include <mbedtls/platform_util.h>
#include <string.h>

const char* ImageSink::reason() const {
    switch (error_) {
        case SinkError::OK: return "ok";
        case SinkError::BAD_ARGUMENT: return "Update refused: bad request";
        case SinkError::ALREADY_ACTIVE: return "Update refused: transfer already running";
        case SinkError::NOT_ACTIVE: return "Update failed: no transfer running";
        case SinkError::NO_PARTITION: return "Update failed: no update slot";
        case SinkError::BAD_SIZE: return header_result_ == ImageHeaderResult::TOO_LARGE
                                             ? "Update refused: image too large"
                                             : "Update refused: image too small";
        case SinkError::BEGIN_FAILED: return "Update failed: cannot prepare flash";
        case SinkError::BAD_HEADER: return image_header_result_text(header_result_);
        case SinkError::TOO_MUCH_DATA: return "Update aborted: too much data";
        case SinkError::WRITE_FAILED: return "Update aborted: flash write failed";
        case SinkError::HASH_FAILED: return "Update aborted: checksum engine error";
        case SinkError::SIZE_MISMATCH: return "Update failed: incomplete image";
        case SinkError::SHA_MISMATCH: return "Update failed: SHA-256 mismatch";
        case SinkError::IMAGE_INVALID: return "Update failed: image rejected";
        case SinkError::BOOT_SET_FAILED: return "Update failed: cannot select new image";
    }
    return "Update failed";
}

SinkError ImageSink::fail(SinkError error) {
    error_ = error;  // abort() keeps error_ and header_result_ so the caller can still read the reason
    LOG_BLE("[UPDATE] sink failure: %s (received %lu of %lu)\n", reason(),
            (unsigned long)received_, (unsigned long)total_);
    abort();
    return error;
}

void ImageSink::reset_state() {
    active_ = false;
    ota_open_ = false;
    header_checked_ = false;
    handle_ = 0;
    partition_ = nullptr;
    total_ = 0;
    received_ = 0;
    head_len_ = 0;
    memset(head_, 0, sizeof(head_));
    memset(expected_sha_, 0, sizeof(expected_sha_));
}

void ImageSink::abort() {
    if (ota_open_) {
        esp_ota_abort(handle_);
        ota_open_ = false;
    }
    if (sha_ready_) {
        mbedtls_sha256_free(&sha_ctx_);  // zeroizes the context
        sha_ready_ = false;
    }
    reset_state();
}

SinkError ImageSink::begin(uint32_t total_size, const char* expected_sha256_hex) {
    if (active_) return SinkError::ALREADY_ACTIVE;  // leaves the running transfer alone
    error_ = SinkError::OK;
    header_result_ = ImageHeaderResult::OK;

    uint8_t sha[SHA256_DIGEST_LEN];
    if (!expected_sha256_hex || !sha256_hex_parse(expected_sha256_hex, sha)) {
        return fail(SinkError::BAD_ARGUMENT);
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) return fail(SinkError::NO_PARTITION);

    if (total_size < UPDATE_IMAGE_MIN_SIZE) {
        header_result_ = ImageHeaderResult::TOO_SMALL;
        return fail(SinkError::BAD_SIZE);
    }
    if (total_size > target->size) {
        header_result_ = ImageHeaderResult::TOO_LARGE;
        return fail(SinkError::BAD_SIZE);
    }

    mbedtls_sha256_init(&sha_ctx_);
    sha_ready_ = true;
    if (mbedtls_sha256_starts(&sha_ctx_, 0) != 0) return fail(SinkError::HASH_FAILED);

    // Erases the announced size up front (can take a few seconds)
    if (esp_ota_begin(target, total_size, &handle_) != ESP_OK) return fail(SinkError::BEGIN_FAILED);

    ota_open_ = true;
    active_ = true;
    partition_ = target;
    total_ = total_size;
    received_ = 0;
    head_len_ = 0;
    header_checked_ = false;
    memcpy(expected_sha_, sha, sizeof(expected_sha_));
    LOG_BLE("[UPDATE] sink begin: %lu bytes into '%s'\n", (unsigned long)total_, target->label);
    return SinkError::OK;
}

// Hashes and flashes bytes that already passed the header check
SinkError ImageSink::write_through(const uint8_t* data, size_t len) {
    if (len == 0) return SinkError::OK;
    if (mbedtls_sha256_update(&sha_ctx_, data, len) != 0) return fail(SinkError::HASH_FAILED);
    if (esp_ota_write(handle_, data, len) != ESP_OK) return fail(SinkError::WRITE_FAILED);
    return SinkError::OK;
}

SinkError ImageSink::write(const uint8_t* data, size_t len) {
    if (!active_) return SinkError::NOT_ACTIVE;
    if (len == 0) return SinkError::OK;
    if (!data) return fail(SinkError::BAD_ARGUMENT);
    if ((uint64_t)received_ + len > total_) return fail(SinkError::TOO_MUCH_DATA);
    received_ += (uint32_t)len;

    // Collect the first bytes (they may arrive in tiny chunks) and validate them before anything is written
    if (!header_checked_) {
        const size_t take = min(len, (size_t)UPDATE_IMAGE_HEADER_BYTES - head_len_);
        memcpy(head_ + head_len_, data, take);
        head_len_ += take;
        data += take;
        len -= take;
        if (head_len_ < UPDATE_IMAGE_HEADER_BYTES) return SinkError::OK;

        header_result_ = image_header_validate(head_, head_len_, total_, partition_->size);
        if (header_result_ != ImageHeaderResult::OK) return fail(SinkError::BAD_HEADER);
        header_checked_ = true;
        const SinkError first = write_through(head_, head_len_);
        if (first != SinkError::OK) return first;
    }
    return write_through(data, len);
}

SinkError ImageSink::finish() {
    if (!active_) return SinkError::NOT_ACTIVE;
    if (received_ != total_) return fail(SinkError::SIZE_MISMATCH);
    if (!header_checked_) {
        header_result_ = ImageHeaderResult::TOO_SHORT;
        return fail(SinkError::BAD_HEADER);
    }

    uint8_t digest[SHA256_DIGEST_LEN];
    if (mbedtls_sha256_finish(&sha_ctx_, digest) != 0) {
        mbedtls_platform_zeroize(digest, sizeof(digest));
        return fail(SinkError::HASH_FAILED);
    }
    const bool sha_ok = sha256_equal(digest, expected_sha_);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    if (!sha_ok) return fail(SinkError::SHA_MISMATCH);

    // esp_ota_end releases the handle whether or not it succeeds
    ota_open_ = false;
    if (esp_ota_end(handle_) != ESP_OK) return fail(SinkError::IMAGE_INVALID);
    if (esp_ota_set_boot_partition(partition_) != ESP_OK) return fail(SinkError::BOOT_SET_FAILED);

    LOG_BLE("[UPDATE] sink finished: '%s' is the next boot partition\n", partition_->label);
    abort();  // releases the SHA context and resets state; the success is the return value
    return SinkError::OK;
}
