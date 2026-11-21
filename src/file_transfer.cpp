/**
 * @file src/file_transfer.cpp
 * @brief File transfer protocol and bounded upload state for paired devices.
 */
// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

// local includes
#include "file_transfer.h"
#include "platform/windows/file_transfer_fs.h"

namespace file_transfer {
  namespace fs = std::filesystem;
  using json = nlohmann::json;
  using clock = std::chrono::steady_clock;
  using namespace std::literals;

  namespace {
    constexpr std::size_t max_settings_bytes = 64 * 1024;  ///< Bound the internal settings read.
    constexpr std::size_t upload_chunk_bytes = 48 * 1024;  ///< Leave room for headers in the GameStream request buffer.
    constexpr std::size_t download_chunk_bytes = 1024 * 1024;  ///< Bound each buffered download reply.
    constexpr std::uint64_t max_upload_bytes = 8ULL * 1024 * 1024 * 1024;  ///< Maximum advertised file size.
    constexpr std::uint64_t max_pending_bytes = 16ULL * 1024 * 1024 * 1024;  ///< Total size reserved by pending uploads.
    constexpr std::size_t page_entries = 100;  ///< Directory entries per response.
    constexpr std::uint64_t max_list_offset = 100000;  ///< Bound work spent skipping earlier directory entries.
    constexpr std::size_t max_jobs = 64;  ///< Bound memory for pending and recently completed jobs.
    constexpr std::size_t max_pending_per_device = 4;  ///< Limit incomplete uploads from each paired device.
    constexpr auto job_timeout = 30min;  ///< Idle time after which upload state is discarded.

    /** @brief A rejected request with a stable client-readable error. */
    struct rejection {
      int status;  ///< HTTP status.
      const char *code;  ///< Machine-readable error identifier.
    };

    /** @brief Host-approved location. */
    struct share {
      std::string label;  ///< Label shown to the paired client.
      fs::path path;  ///< Absolute local path, private to the host.
    };

    /** @brief Effective host settings loaded from disk for each request. */
    struct settings {
      std::map<std::string, share, std::less<>> roots;  ///< Share identifier to configured location.
      fs::path inbox;  ///< Empty when receiving files is disabled.
      std::optional<std::vector<std::string>> devices;  ///< Explicit allowed paired-device IDs, if configured.

      /** @brief Check whether the paired device may access files. */
      bool allows(std::string_view device) const {
        return !device.empty() && (!devices || std::ranges::find(*devices, device) != devices->end());
      }
    };

    /** @brief Remember ownership and the last acknowledged upload offset. */
    struct upload_job {
      std::string owner;  ///< UUID of the paired client that opened the upload.
      std::string name;  ///< Final Windows filename.
      fs::path inbox;  ///< Folder authorized when the upload was created.
      std::uint64_t size;  ///< Declared number of bytes.
      std::uint64_t offset = 0;  ///< Number of bytes accepted from the client.
      clock::time_point touched = clock::now();  ///< Last successful request.
      std::unique_ptr<storage::upload> file;  ///< Pending file, null once published.
    };

    /** @brief Serialize a JSON response with caching disabled. */
    reply json_reply(int status, json body) {
      return {status, body.dump(), {{"Content-Type", "application/json"}, {"Cache-Control", "no-store"}}};
    }

    /** @brief Read a unique query value or return the supplied fallback. */
    std::string_view arg(const query_t &query, std::string_view key, std::string_view fallback = {}) {
      const auto [first, last] = query.equal_range(key);
      if (first == last) {
        return fallback;
      }
      if (std::next(first) != last) {
        throw rejection {400, "invalid_query"};
      }
      return first->second;
    }

    /** @brief Parse an unsigned base-10 query value. */
    std::uint64_t number_arg(const query_t &query, std::string_view key, std::string_view fallback = {}) {
      const auto value = arg(query, key, fallback);
      std::uint64_t result = 0;
      if (value.empty()) {
        throw rejection {400, "invalid_query"};
      }
      const auto end = value.data() + value.size();
      const auto [parsed, error] = std::from_chars(value.data(), end, result);
      if (error != std::errc {} || parsed != end) {
        throw rejection {400, "invalid_query"};
      }
      return result;
    }

    /** @brief Validate a stable share identifier safe to expose in URLs. */
    bool valid_root_id(std::string_view text) {
      return !text.empty() && text.size() <= 64 && std::ranges::all_of(text, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      });
    }

    /** @brief Convert a UTF-8 configured path into the platform filesystem representation. */
    fs::path parse_path(const json &value) {
      const auto text = value.get<std::string>();
      if (text.empty() || text.find('\0') != std::string::npos) {
        throw rejection {503, "invalid_configuration"};
      }
      const fs::path path {std::u8string(text.begin(), text.end())};
      if (!path.is_absolute()) {
        throw rejection {503, "invalid_configuration"};
      }
      return path;
    }

    /** @brief Load the opt-in share settings or return an empty disabled configuration. */
    settings read_settings(const fs::path &path) {
      std::ifstream stream {path, std::ios::binary};
      if (!stream) {
        std::error_code ec;
        if (!fs::exists(path, ec) && !ec) {
          return {};
        }
        throw rejection {503, "invalid_configuration"};
      }
      std::string data(max_settings_bytes + 1, '\0');
      stream.read(data.data(), static_cast<std::streamsize>(data.size()));
      if (stream.bad() || stream.gcount() > static_cast<std::streamsize>(max_settings_bytes)) {
        throw rejection {503, "invalid_configuration"};
      }
      data.resize(stream.gcount());

      try {
        const auto document = json::parse(data);
        if (!document.is_object()) {
          throw rejection {503, "invalid_configuration"};
        }
        settings config;
        if (document.contains("devices")) {
          config.devices = document.at("devices").get<std::vector<std::string>>();
        }
        if (document.contains("inbox")) {
          config.inbox = parse_path(document.at("inbox"));
        }
        if (document.contains("roots")) {
          const auto &roots = document.at("roots");
          if (!roots.is_array()) {
            throw rejection {503, "invalid_configuration"};
          }
          for (const auto &item : roots) {
            const auto id = item.at("id").get<std::string>();
            const auto label = item.at("label").get<std::string>();
            if (!valid_root_id(id) || label.empty() || label.size() > 256 ||
                !config.roots.emplace(id, share {label, parse_path(item.at("path"))}).second) {
              throw rejection {503, "invalid_configuration"};
            }
          }
        }
        return config;
      } catch (const json::exception &) {
        throw rejection {503, "invalid_configuration"};
      } catch (const fs::filesystem_error &) {
        throw rejection {503, "invalid_configuration"};
      }
    }

    /** @brief Return the public fields of an upload without exposing the owner's identity. */
    json upload_status(std::string_view id, const upload_job &job) {
      return {{"id", id}, {"name", job.name}, {"size", job.size}, {"offset", job.offset}, {"complete", !job.file}};
    }

    /** @brief Reject uploads with an unexpected media type. */
    void require_type(std::string_view actual, std::string_view expected) {
      if (actual.substr(0, actual.find(';')) != expected) {
        throw rejection {415, "invalid_content_type"};
      }
    }

    /** @brief Generate an unguessable upload ID and reserved staging filename suffix. */
    std::string new_upload_id() {
      std::array<unsigned char, 16> bytes {};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw rejection {500, "file_io_error"};
      }
      bytes[6] = (bytes[6] & 0x0f) | 0x40;
      bytes[8] = (bytes[8] & 0x3f) | 0x80;
      constexpr std::string_view hex = "0123456789abcdef";
      std::string id;
      id.reserve(36);
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
          id += '-';
        }
        id += hex[bytes[i] >> 4];
        id += hex[bytes[i] & 0x0f];
      }
      return id;
    }

    /** @brief Map filesystem failures to public status codes. */
    rejection storage_rejection(storage::error_code code) {
      using enum storage::error_code;
      switch (code) {
        case invalid: return {400, "invalid_path"};
        case not_found: return {404, "not_found"};
        case forbidden: return {403, "path_forbidden"};
        case changed: return {409, "file_changed"};
        case conflict: return {409, "file_exists"};
        case io: return {500, "file_io_error"};
      }
      return {500, "file_io_error"};
    }
  }  // namespace

  /** @brief Bounds the upload queue and serializes each job's filesystem updates. */
  struct service::impl {
    fs::path settings_file;  ///< Local file configured by the Windows user.
    std::mutex mutex;  ///< Protects the upload map from concurrent requests.
    std::map<std::string, upload_job, std::less<>> jobs;  ///< Upload state scoped to the current engine process.

    /** @brief Initialize storage for the given host settings path. */
    explicit impl(fs::path path): settings_file {std::move(path)} {}

    /** @brief Discard expired jobs and files whose owner or inbox has lost access. */
    void prune(const settings &config) {
      const auto now = clock::now();
      std::erase_if(jobs, [&](const auto &item) {
        const auto &job = item.second;
        return job.touched + job_timeout <= now || job.inbox != config.inbox || !config.allows(job.owner);
      });
    }

    /** @brief Start a staged upload under the host's configured inbox. */
    reply create(std::string_view device, const settings &config, std::string_view type, std::string_view body) {
      if (config.inbox.empty()) {
        throw rejection {403, "upload_disabled"};
      }
      require_type(type, "application/json");
      if (body.size() > 4096) {
        throw rejection {413, "request_too_large"};
      }
      const auto request = json::parse(body);
      const auto name = request.at("name").get<std::string>();
      const auto &bytes = request.at("size");
      if (!bytes.is_number_unsigned()) {
        throw rejection {400, "invalid_size"};
      }
      const auto size = bytes.get<std::uint64_t>();
      if (size > max_upload_bytes) {
        throw rejection {413, "file_too_large"};
      }
      std::size_t owner_pending = 0;
      std::uint64_t pending_bytes = 0;
      auto oldest = jobs.end();
      for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        const auto &job = it->second;
        if (job.file) {
          pending_bytes += job.size;
          owner_pending += job.owner == device;
        } else if (oldest == jobs.end() || job.touched < oldest->second.touched) {
          oldest = it;
        }
      }
      if (owner_pending >= max_pending_per_device || pending_bytes > max_pending_bytes - size) {
        throw rejection {429, "upload_limit"};
      }
      if (jobs.size() >= max_jobs) {
        if (oldest == jobs.end()) {
          throw rejection {429, "upload_limit"};
        }
        jobs.erase(oldest);
      }
      auto id = new_upload_id();
      while (jobs.contains(id)) {
        id = new_upload_id();
      }
      auto file = std::make_unique<storage::upload>(config.inbox, name, id);
      const auto it = jobs.try_emplace(id, upload_job {
        .owner = std::string {device},
        .name = name,
        .inbox = config.inbox,
        .size = size,
        .file = std::move(file),
      }).first;
      return json_reply(201, upload_status(id, it->second));
    }

    /** @brief Inspect, append to, finalize, or discard an owned upload. */
    reply handle_upload(std::string_view device, operation action, const query_t &query, std::string_view type, std::string_view body) {
      const auto id = arg(query, "id");
      const auto it = jobs.find(id);
      if (it == jobs.end() || it->second.owner != device) {
        throw rejection {404, "upload_not_found"};
      }
      auto &job = it->second;
      if (action == operation::cancel_upload) {
        if (!job.file) {
          throw rejection {409, "upload_already_complete"};
        }
        jobs.erase(it);
        return json_reply(200, {{"cancelled", true}});
      }
      if (action == operation::append_upload) {
        if (!job.file) {
          throw rejection {409, "upload_already_complete"};
        }
        require_type(type, "application/octet-stream");
        const auto offset = number_arg(query, "offset");
        if (offset != job.offset) {
          throw rejection {409, "offset_mismatch"};
        }
        if (body.size() > upload_chunk_bytes) {
          throw rejection {413, "chunk_too_large"};
        }
        if (body.empty() || body.size() > job.size - job.offset) {
          throw rejection {400, "invalid_chunk_size"};
        }
        try {
          job.file->append(offset, body);
        } catch (const storage::error &) {
          jobs.erase(it);
          throw;
        }
        job.offset += body.size();
      } else if (action == operation::complete_upload && job.file) {
        if (job.offset != job.size) {
          throw rejection {409, "upload_incomplete"};
        }
        try {
          job.file->commit();
        } catch (const storage::error &) {
          jobs.erase(it);
          throw;
        }
        job.file.reset();
      }
      job.touched = clock::now();
      return json_reply(200, upload_status(id, job));
    }
  };

  service::service(fs::path settings_file): data {std::make_unique<impl>(std::move(settings_file))} {}

  service::~service() = default;

  void service::revoke(std::string_view device) {
    std::lock_guard lock {data->mutex};
    std::erase_if(data->jobs, [&](const auto &item) { return item.second.owner == device; });
  }

  reply service::execute(std::string_view device, operation action, const query_t &query, std::string_view type, std::string_view body) {
    std::lock_guard lock {data->mutex};
    try {
      const auto config = read_settings(data->settings_file);
      data->prune(config);
      if (!config.allows(device)) {
        throw rejection {403, "device_forbidden"};
      }
      const auto enabled = !config.inbox.empty() || !config.roots.empty();
      if (action == operation::capabilities) {
        auto roots = json::array();
        for (const auto &[id, root] : config.roots) {
          roots.push_back({{"id", id}, {"label", root.label}});
        }
        return json_reply(200, {
          {"version", 1}, {"enabled", enabled}, {"roots", std::move(roots)}, {"upload", !config.inbox.empty()},
          {"upload_chunk_bytes", upload_chunk_bytes}, {"download_chunk_bytes", download_chunk_bytes},
          {"max_upload_bytes", max_upload_bytes}, {"upload_idle_seconds", std::chrono::duration_cast<std::chrono::seconds>(job_timeout).count()},
        });
      }
      if (!enabled) {
        throw rejection {403, "file_transfer_disabled"};
      }
      if (action == operation::list || action == operation::download) {
        const auto root = config.roots.find(arg(query, "root"));
        if (root == config.roots.end()) {
          throw rejection {404, "root_not_found"};
        }
        const auto path = arg(query, "path");
        const auto offset = number_arg(query, "offset", "0");
        if (path.size() > 4096) {
          throw rejection {400, "invalid_path"};
        }
        if (action == operation::list) {
          if (offset > max_list_offset) {
            throw rejection {400, "invalid_query"};
          }
          const auto page = storage::list(root->second.path, path, offset, page_entries);
          auto entries = json::array();
          for (const auto &entry : page.entries) {
            entries.push_back({{"name", entry.name}, {"kind", entry.directory ? "directory" : "file"},
                               {"size", entry.size}, {"modified_unix_ms", entry.modified_unix_ms}, {"version", entry.version}});
          }
          const auto next = page.next_offset && *page.next_offset <= max_list_offset ? json(*page.next_offset) : json(nullptr);
          return json_reply(200, {{"entries", std::move(entries)}, {"next_offset", next},
                                 {"truncated", page.next_offset && *page.next_offset > max_list_offset}});
        }
        const auto length = number_arg(query, "length", "1048576");
        const auto version = arg(query, "version");
        if (length == 0 || length > download_chunk_bytes || (offset != 0 && version.empty())) {
          throw rejection {400, "invalid_query"};
        }
        auto chunk = storage::read(root->second.path, path, offset, static_cast<std::size_t>(length), version);
        return {200, std::move(chunk.bytes), {
          {"Content-Type", "application/octet-stream"}, {"Cache-Control", "no-store"},
          {"X-SideLantern-File-Size", std::to_string(chunk.size)}, {"X-SideLantern-File-Version", std::move(chunk.version)},
        }};
      }
      if (action == operation::create_upload) {
        return data->create(device, config, type, body);
      }
      return data->handle_upload(device, action, query, type, body);
    } catch (const storage::error &error) {
      const auto mapped = storage_rejection(error.code);
      return json_reply(mapped.status, {{"error", mapped.code}});
    } catch (const rejection &error) {
      return json_reply(error.status, {{"error", error.code}});
    } catch (const json::exception &) {
      return json_reply(400, {{"error", "invalid_request"}});
    }
  }
}  // namespace file_transfer
