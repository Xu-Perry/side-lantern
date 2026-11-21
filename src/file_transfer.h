/**
 * @file src/file_transfer.h
 * @brief Paired-device file transfer operations for the Windows host.
 */
#pragma once

// standard includes
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace file_transfer {
  /** @brief An operation exposed to a paired client on HTTPS. */
  enum class operation {
    capabilities,
    list,
    download,
    create_upload,
    get_upload,
    append_upload,
    complete_upload,
    cancel_upload,
  };

  using query_t = std::multimap<std::string, std::string, std::less<>>;  ///< Decoded query parameters, including duplicates.

  /** @brief Transport-independent HTTP reply. */
  struct reply {
    int status;  ///< HTTP status.
    std::string body;  ///< UTF-8 JSON or binary file data.
    std::map<std::string, std::string> headers;  ///< Response headers that accompany the body.
  };

  /** @brief Owns pending uploads and checks the current host shares for every request. */
  class service {
  public:
    /** @brief Create a file transfer service using the given optional host settings file. */
    explicit service(std::filesystem::path settings_file);

    /** @brief Release pending uploads and their temporary files. */
    ~service();

    /**
     * @brief Execute a file operation for a device verified by the HTTPS listener.
     * @param device UUID of the enabled paired client.
     * @param action Operation selected by the HTTP route.
     * @param query Decoded URL parameters.
     * @param content_type Content-Type header, if present.
     * @param body Request payload.
     * @return HTTP response without paths or private host settings.
     */
    reply execute(std::string_view device, operation action, const query_t &query = {}, std::string_view content_type = {}, std::string_view body = {});

    /** @brief Cancel a device's pending uploads after its pairing is removed or disabled. */
    void revoke(std::string_view device);

  private:
    struct impl;  ///< Service state shared by the HTTP handlers.
    std::unique_ptr<impl> data;  ///< Owns the upload state for this listener.
  };
}  // namespace file_transfer
