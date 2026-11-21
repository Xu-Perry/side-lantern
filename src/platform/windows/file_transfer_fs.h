/**
 * @file src/platform/windows/file_transfer_fs.h
 * @brief Windows filesystem operations for authorized file transfers.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace file_transfer::storage {
  /** @brief Storage failures that can be reported without disclosing local paths. */
  enum class error_code {
    invalid,  ///< Invalid path, range, or operation.
    not_found,  ///< The requested file or directory does not exist.
    forbidden,  ///< Access denied or the path is unsafe to open.
    changed,  ///< The requested file revision no longer matches.
    conflict,  ///< The upload would overwrite an existing file.
    io,  ///< Other filesystem failure.
  };

  /** @brief A filesystem failure with a client-safe category. */
  class error: public std::runtime_error {
  public:
    /**
     * @brief Report a filesystem failure without including its path.
     * @param code Category of the failure.
     */
    error(error_code code);

    const error_code code;  ///< Failure category.
  };

  /** @brief One child in a directory listing. */
  struct entry {
    std::string name;  ///< UTF-8 child name.
    bool directory;  ///< Whether the child is a directory.
    uint64_t size;  ///< File size in bytes, or zero for a directory.
    std::string version;  ///< Opaque revision of a regular file.
    std::int64_t modified_unix_ms = 0;  ///< Last modification time in Unix milliseconds.
  };

  /** @brief A bounded directory listing and its continuation offset. */
  struct page {
    std::vector<entry> entries;  ///< Requested children in enumeration order.
    std::optional<uint64_t> next_offset;  ///< Offset of the next page, if present.
  };

  /**
   * @brief Enumerate children of an authorized Windows directory.
   * @param root Absolute, local path configured by the host.
   * @param relative UTF-8 path beneath the root with slash separators, or empty for the root.
   * @param offset Number of visible children to skip.
   * @param limit Maximum number of children to return.
   * @return A listing page with a continuation if more entries exist.
   */
  page list(const std::filesystem::path &root, std::string_view relative, uint64_t offset, size_t limit);

  /** @brief A byte range with information identifying the source file. */
  struct download {
    std::string bytes;  ///< Bytes from the requested range.
    uint64_t size;  ///< Full file size in bytes.
    std::string version;  ///< Opaque revision of the file.
  };

  /**
   * @brief Read a range from a regular file under an authorized Windows directory.
   * @param root Absolute, local path configured by the host.
   * @param relative UTF-8 path to the file, with slash separators.
   * @param offset Byte offset from which to start reading.
   * @param limit Maximum number of bytes to read.
   * @param version Expected file revision, or empty to accept the current revision.
   * @return File bytes and corresponding metadata.
   */
  download read(const std::filesystem::path &root, std::string_view relative, uint64_t offset, size_t limit, std::string_view version);

  /** @brief A pending upload in a pinned inbox directory. */
  class upload {
  public:
    /**
     * @brief Create an exclusive pending file in an authorized inbox.
     * @param inbox Absolute, local path configured by the host.
     * @param name Destination UTF-8 file name.
     * @param id Random upload UUID used for the private staging name.
     */
    upload(const std::filesystem::path &inbox, std::string_view name, std::string_view id);

    /** @brief Close the pending file and discard any unpublished data. */
    ~upload();
    upload(const upload &) = delete;
    upload &operator=(const upload &) = delete;

    /**
     * @brief Append sequential data to the pending file.
     * @param offset Expected byte offset, which must match the staged length.
     * @param data Bytes to append.
     */
    void append(uint64_t offset, std::string_view data);

    /** @brief Flush and publish the complete file without replacing another file. */
    void commit();

  private:
    struct impl;  ///< Native handles and pending upload state.
    std::unique_ptr<impl> data;  ///< Owns staged file and pinned directories.
  };
}  // namespace file_transfer::storage
