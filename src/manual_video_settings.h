/**
 * @file src/manual_video_settings.h
 * @brief Per-session settings requested by a paired SideLantern client during streaming.
 */
#pragma once

#include <mutex>
#include <optional>
#include <string_view>

namespace video {
  /** @brief Encoding settings that can change without changing the negotiated codec or capture target. */
  struct video_settings_t {
    int width;  ///< Encoded width in pixels.
    int height;  ///< Encoded height in pixels.
    int fps;  ///< Target capture and encoding frame rate.
    int bitrate_kbps;  ///< Target video encoder bitrate, excluding transport overhead.

    /** @brief Compare every setting. */
    bool operator==(const video_settings_t &) const = default;
  };

  /** @brief Serialize settings requests from HTTPS and their application on the encoder thread. */
  class manual_video_settings_t {
  public:
    /** @brief Point-in-time response to a client query. */
    struct snapshot_t {
      video_settings_t current;  ///< Last settings accepted by the encoder.
      std::string_view status;  ///< Result of the last request, or pending while it is queued.
    };

    /** @brief Initialize with the negotiated settings. @param initial Initial settings. */
    explicit manual_video_settings_t(video_settings_t initial): current {initial} {}

    /** @brief Queue a single request. @param settings Requested settings. @return False if a request is pending. */
    bool request(video_settings_t settings) {
      std::lock_guard lock {mutex};
      if (queued) {
        return false;
      }
      if (current == settings) {
        status = "applied";
        return true;
      }
      queued = settings;
      status = "pending";
      return true;
    }

    /** @brief Copy the pending settings for the encoder thread. @return Pending settings if present. */
    [[nodiscard]] std::optional<video_settings_t> pending() const {
      std::lock_guard lock {mutex};
      return queued;
    }

    /** @brief Publish a successful change or discard a failed encoder replacement. @param applied Encoder result. */
    void finish(bool applied) {
      std::lock_guard lock {mutex};
      if (!queued) {
        return;
      }
      if (applied) {
        current = *queued;
      }
      queued.reset();
      status = applied ? "applied" : "rejected";
    }

    /** @brief Read the currently applied settings and the status of the latest request. @return Current snapshot. */
    [[nodiscard]] snapshot_t snapshot() const {
      std::lock_guard lock {mutex};
      return {current, status};
    }

  private:
    mutable std::mutex mutex;  ///< Protects settings and request result across worker threads.
    video_settings_t current;  ///< Active encoding settings.
    std::optional<video_settings_t> queued;  ///< Unacknowledged encoder request.
    std::string_view status {"idle"};  ///< Most recent result.
  };
}  // namespace video
