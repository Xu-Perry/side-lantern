/**
 * @file src/platform/windows/file_transfer_fs.cpp
 * @brief Handle-relative Windows filesystem operations for authorized file transfers.
 */
#include "file_transfer_fs.h"

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <utility>

// lib includes
#include <Windows.h>
#include <winternl.h>

namespace file_transfer::storage {
  namespace {
    constexpr auto stage_prefix = std::wstring_view {L".sidelantern-upload-"};
    constexpr auto max_component = 255;
    constexpr auto directory_access = FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    constexpr auto directory_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;

    /** @brief Own and close one Windows file handle. */
    struct file_closer {
      void operator()(void *handle) const {
        CloseHandle(handle);
      }
    };

    using file_handle = std::unique_ptr<void, file_closer>;
    using directory_chain = std::vector<file_handle>;

    /** @brief Resolve the native APIs used for handle-relative filesystem operations. */
    struct native_api {
      HMODULE module = GetModuleHandleW(L"ntdll.dll");  ///< Already-loaded Windows runtime.
      decltype(&NtCreateFile) open = std::bit_cast<decltype(open)>(GetProcAddress(module, "NtCreateFile"));  ///< Open or create a child.
      decltype(&NtSetInformationFile) set_info = std::bit_cast<decltype(set_info)>(GetProcAddress(module, "NtSetInformationFile"));  ///< Rename a staged file.
      decltype(&RtlNtStatusToDosError) to_win32 = std::bit_cast<decltype(to_win32)>(GetProcAddress(module, "RtlNtStatusToDosError"));  ///< Translate a native status code.
    };

    /** @brief Return the native filesystem functions or fail closed. */
    const native_api &native() {
      static const native_api api;
      if (!api.open || !api.set_info || !api.to_win32) {
        throw error {error_code::io};
      }
      return api;
    }

    /** @brief Translate a Windows error without exposing the failing path. */
    [[noreturn]] void fail(DWORD windows_error) {
      switch (windows_error) {
        case ERROR_INVALID_NAME:
        case ERROR_FILENAME_EXCED_RANGE:
          throw error {error_code::invalid};
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_DIRECTORY:
        case ERROR_NOT_READY:
          throw error {error_code::not_found};
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
          throw error {error_code::conflict};
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_DELETE_PENDING:
        case ERROR_CANT_ACCESS_FILE:
        case ERROR_STOPPED_ON_SYMLINK:
          throw error {error_code::forbidden};
        default:
          throw error {error_code::io};
      }
    }

    /** @brief Compare filenames with Windows ordinal case folding. */
    bool equals_ordinal(std::wstring_view left, std::wstring_view right) {
      return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    /** @brief Determine if a single Windows path component can be exposed. */
    bool valid_component(std::wstring_view name) {
      if (name.empty() || name.size() > max_component || name.back() == L'.' || name.back() == L' ' || equals_ordinal(name.substr(0, stage_prefix.size()), stage_prefix)) {
        return false;
      }
      constexpr auto special = std::wstring_view {L"<>:\"/\\|?*"};
      if (std::ranges::any_of(name, [&](wchar_t character) {
            return character < 0x20 || (character >= 0x7f && character <= 0x9f) || special.find(character) != std::wstring_view::npos;
          })) {
        return false;
      }
      auto stem = name.substr(0, name.find(L'.'));
      while (!stem.empty() && stem.back() == L' ') {
        stem.remove_suffix(1);
      }
      for (const std::wstring_view device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"CLOCK$"}) {
        if (equals_ordinal(stem, device)) {
          return false;
        }
      }
      constexpr auto device_digits = std::wstring_view {L"123456789\u00b9\u00b2\u00b3"};
      return !(stem.size() == 4 && (equals_ordinal(stem.substr(0, 3), L"COM") || equals_ordinal(stem.substr(0, 3), L"LPT")) && device_digits.find(stem.back()) != std::wstring_view::npos);
    }

    /** @brief Decode a network path and reject malformed UTF-8. */
    std::wstring decode(std::string_view name) {
      if (name.empty()) {
        return {};
      }
      if (name.size() > 32767) {
        throw error {error_code::invalid};
      }
      const auto bytes = static_cast<int>(name.size());
      const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), bytes, nullptr, 0);
      std::wstring result(count, L'\0');
      if (count == 0 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), bytes, result.data(), count) != count) {
        throw error {error_code::invalid};
      }
      return result;
    }

    /** @brief Encode a Windows component only when its UTF-16 is valid. */
    std::optional<std::string> encode(std::wstring_view name) {
      const auto length = static_cast<int>(name.size());
      const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, nullptr, 0, nullptr, nullptr);
      std::string result(count, '\0');
      if (count == 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, result.data(), count, nullptr, nullptr) != count) {
        return std::nullopt;
      }
      return result;
    }

    /** @brief Validate and split a path into components for individual native opens. */
    std::vector<std::wstring_view> components(std::wstring_view path, std::wstring_view separators) {
      std::vector<std::wstring_view> result;
      while (!path.empty()) {
        const auto separator = path.find_first_of(separators);
        const auto component = path.substr(0, separator);
        if (!valid_component(component) || !encode(component)) {
          throw error {error_code::invalid};
        }
        result.push_back(component);
        if (separator == std::wstring_view::npos) {
          break;
        }
        path.remove_prefix(separator + 1);
        if (path.empty()) {
          throw error {error_code::invalid};
        }
      }
      return result;
    }

    /** @brief Query typed metadata from a pinned handle. */
    template<typename T>
    T info(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind) {
      T result {};
      if (!GetFileInformationByHandleEx(handle, kind, &result, sizeof(result))) {
        fail(GetLastError());
      }
      return result;
    }

    /** @brief Require a local disk object with no reparse behavior. */
    void verify_disk_handle(HANDLE handle) {
      if (GetFileType(handle) != FILE_TYPE_DISK || (info<FILE_ATTRIBUTE_TAG_INFO>(handle, FileAttributeTagInfo).FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw error {error_code::forbidden};
      }
    }

    /** @brief Open or create one already-validated name relative to a pinned directory. */
    file_handle open_child(HANDLE parent, std::wstring_view name, ACCESS_MASK access, ULONG sharing, ULONG disposition, ULONG options) {
      const auto length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
      UNICODE_STRING filename {length, length, const_cast<wchar_t *>(name.data())};
      OBJECT_ATTRIBUTES attributes {};
      InitializeObjectAttributes(&attributes, &filename, OBJ_CASE_INSENSITIVE, parent, nullptr);
      IO_STATUS_BLOCK status_block {};
      HANDLE handle = nullptr;
      const auto &api = native();
      const auto status = api.open(&handle, access | SYNCHRONIZE, &attributes, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, sharing, disposition, options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
      if (status < 0) {
        fail(api.to_win32(status));
      }
      file_handle result {handle};
      verify_disk_handle(handle);
      return result;
    }

    /** @brief Recognize the separators permitted in a configured DOS drive path. */
    bool root_separator(wchar_t value) {
      return value == L'\\' || value == L'/';
    }

    /** @brief Resolve a mounted local drive letter without following network or DOS drive aliases. */
    bool local_volume_name(const wchar_t *drive, wchar_t (&volume)[MAX_PATH]) {
      const auto type = GetDriveTypeW(drive);
      return (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK) &&
             GetVolumeNameForVolumeMountPointW(drive, volume, MAX_PATH);
    }

    /** @brief Check whether a Windows object is the excluded directory. */
    bool is_private(std::uint64_t volume, const FILE_ID_128 &id, const FILE_ID_INFO *private_id) {
      return private_id && volume == private_id->VolumeSerialNumber &&
             std::ranges::equal(id.Identifier, private_id->FileId.Identifier);
    }

    /** @brief Prevent traversal into app data by directory identity, including alternate spellings. */
    void require_public(HANDLE directory, const FILE_ID_INFO *private_id) {
      if (private_id) {
        const auto id = info<FILE_ID_INFO>(directory, FileIdInfo);
        if (is_private(id.VolumeSerialNumber, id.FileId, private_id)) {
          throw error {error_code::forbidden};
        }
      }
    }

    /** @brief Pin the actual volume root and every directory up to the destination. */
    directory_chain open_directories(const std::filesystem::path &root, const std::vector<std::wstring_view> &relative, ACCESS_MASK final_access = 0, const FILE_ID_INFO *private_id = nullptr) {
      const std::wstring_view path = root.native();
      if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' || !root_separator(path[2])) {
        throw error {error_code::invalid};
      }
      auto subpath = path.substr(3);
      if (!subpath.empty() && root_separator(subpath.back())) {
        subpath.remove_suffix(1);
        if (subpath.empty()) {
          throw error {error_code::invalid};
        }
      }
      auto names = components(subpath, L"\\/");
      names.insert(names.end(), relative.begin(), relative.end());

      const wchar_t drive[] {path[0], L':', L'\\', L'\0'};
      // A volume GUID names the mounted drive itself and avoids DOS device aliases.
      wchar_t volume[MAX_PATH] {};
      if (!local_volume_name(drive, volume)) {
        throw error {error_code::forbidden};
      }
      const auto handle = CreateFileW(volume, directory_access | (names.empty() ? final_access : 0), directory_sharing, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        fail(GetLastError());
      }
      file_handle volume_root {handle};
      verify_disk_handle(handle);
      require_public(handle, private_id);
      directory_chain result;
      result.reserve(names.size() + 1);
      result.push_back(std::move(volume_root));
      for (size_t index = 0; index < names.size(); ++index) {
        const auto access = directory_access | (index + 1 == names.size() ? final_access : 0);
        result.push_back(open_child(result.back().get(), names[index], access, directory_sharing, FILE_OPEN, FILE_DIRECTORY_FILE));
        require_public(result.back().get(), private_id);
      }
      return result;
    }

    /** @brief Keep the excluded directory stable during a browse or download request. */
    struct private_directory_guard {
      directory_chain handles;  ///< Pin the private directory and its ancestors.
      std::optional<FILE_ID_INFO> id;  ///< Private directory identity, if applicable.

      /** @brief Resolve the private directory once for this operation. */
      explicit private_directory_guard(const std::filesystem::path &path) {
        if (!path.empty()) {
          handles = open_directories(path, {});
          id = info<FILE_ID_INFO>(handles.back().get(), FileIdInfo);
        }
      }

      /** @brief Obtain the identity for traversal checks. */
      const FILE_ID_INFO *get() const {
        return id ? &*id : nullptr;
      }
    };

    /** @brief Append fixed-width hexadecimal bytes to an opaque revision string. */
    void append_hex(std::string &result, const auto &value) {
      constexpr auto digits = "0123456789abcdef";
      for (const auto byte : std::bit_cast<std::array<uint8_t, sizeof(value)>>(value)) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
      }
    }

    /** @brief Identify a particular file and revision on its volume. */
    std::string revision(uint64_t serial, const FILE_ID_128 &id, uint64_t size, LARGE_INTEGER created, LARGE_INTEGER written, LARGE_INTEGER changed) {
      std::string result;
      result.reserve(112);
      append_hex(result, serial);
      append_hex(result, id.Identifier);
      append_hex(result, size);
      append_hex(result, created.QuadPart);
      append_hex(result, written.QuadPart);
      append_hex(result, changed.QuadPart);
      return result;
    }

    /** @brief Convert Windows 100-nanosecond timestamps into Unix milliseconds. */
    std::int64_t modified_unix_ms(LARGE_INTEGER written) {
      constexpr std::int64_t unix_epoch_ms = 11644473600000;
      return written.QuadPart / 10000 - unix_epoch_ms;
    }

    /** @brief Size and revision collected from a readable, single-link regular file. */
    struct file_revision {
      uint64_t size;  ///< Size observed on the open file handle.
      std::string version;  ///< Revision token observed on the open file handle.
    };

    /** @brief Read revision metadata and reject hard links and special files. */
    file_revision verify_file(HANDLE file) {
      const auto standard = info<FILE_STANDARD_INFO>(file, FileStandardInfo);
      const auto basic = info<FILE_BASIC_INFO>(file, FileBasicInfo);
      if (standard.Directory || standard.DeletePending || standard.NumberOfLinks != 1 || (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw error {error_code::forbidden};
      }
      const auto id = info<FILE_ID_INFO>(file, FileIdInfo);
      const auto size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
      return {size, revision(id.VolumeSerialNumber, id.FileId, size, basic.CreationTime, basic.LastWriteTime, basic.ChangeTime)};
    }

    /** @brief Set an absolute byte offset on a synchronous file handle. */
    void seek(HANDLE file, uint64_t offset) {
      LARGE_INTEGER position {};
      position.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        fail(GetLastError());
      }
    }

    /** @brief Recognize the upload UUID format before using it in a native path. */
    bool valid_upload_id(std::string_view id) {
      if (id.size() != 36) {
        return false;
      }
      constexpr auto hex = std::string_view {"0123456789abcdefABCDEF"};
      for (size_t index = 0; index < id.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? id[index] != '-' : hex.find(id[index]) == std::string_view::npos) {
          return false;
        }
      }
      return true;
    }

    /** @brief Publish under a single name within the pinned inbox, with replacement disabled. */
    void rename_upload(HANDLE file, HANDLE inbox, std::wstring_view name) {
      struct {
        BOOLEAN replace;
        HANDLE directory;
        ULONG name_length;
        wchar_t name[max_component];
      } rename {};
      static_assert(offsetof(decltype(rename), name) == offsetof(FILE_RENAME_INFO, FileName));
      rename.directory = inbox;
      rename.name_length = static_cast<ULONG>(name.size() * sizeof(wchar_t));
      std::ranges::copy(name, rename.name);
      IO_STATUS_BLOCK status_block {};
      const auto &api = native();
      const auto status = api.set_info(file, &status_block, &rename, sizeof(rename), FileRenameInformation);
      if (status < 0) {
        fail(api.to_win32(status));
      }
    }
  }  // namespace

  error::error(error_code code):
      std::runtime_error {"File transfer storage error"},
      code {code} {
  }

  std::vector<std::filesystem::path> local_drive_roots() {
    std::vector<std::filesystem::path> result;
    const auto drives = GetLogicalDrives();
    for (unsigned int bit = 0; bit < 26; ++bit) {
      if (drives & (1UL << bit)) {
        const wchar_t drive[] {static_cast<wchar_t>(L'A' + bit), L':', L'\\', L'\0'};
        wchar_t volume[MAX_PATH] {};
        if (local_volume_name(drive, volume)) {
          result.emplace_back(drive);
        }
      }
    }
    return result;
  }

  page list(const std::filesystem::path &root, std::string_view relative, uint64_t offset, size_t limit, const std::filesystem::path &private_directory) {
    if (limit == 0) {
      throw error {error_code::invalid};
    }
    const auto path = decode(relative);
    const private_directory_guard private_scope {private_directory};
    const auto directories = open_directories(root, components(path, L"/"), FILE_LIST_DIRECTORY, private_scope.get());
    const auto directory = directories.back().get();
    const auto serial = info<FILE_ID_INFO>(directory, FileIdInfo).VolumeSerialNumber;
    alignas(FILE_ID_EXTD_DIR_INFO) std::array<std::byte, 16384> buffer;
    uint64_t skipped = 0;
    page result;
    while (GetFileInformationByHandleEx(directory, FileIdExtdDirectoryInfo, buffer.data(), static_cast<DWORD>(buffer.size()))) {
      const auto *record = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO *>(buffer.data());
      while (true) {
        const auto attributes = record->FileAttributes;
        const std::wstring_view name {record->FileName, record->FileNameLength / sizeof(wchar_t)};
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && valid_component(name) &&
            !is_private(serial, record->FileId, private_scope.get())) {
          if (auto encoded = encode(name)) {
            if (skipped < offset) {
              ++skipped;
            } else {
              if (result.entries.size() == limit) {
                result.next_offset = offset + limit;
                return result;
              }
              const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
              const auto size = is_directory ? 0 : static_cast<uint64_t>(record->EndOfFile.QuadPart);
              result.entries.push_back({
                std::move(*encoded),
                is_directory,
                size,
                is_directory ? std::string {} : revision(serial, record->FileId, size, record->CreationTime, record->LastWriteTime, record->ChangeTime),
                modified_unix_ms(record->LastWriteTime),
              });
            }
          }
        }
        if (record->NextEntryOffset == 0) {
          break;
        }
        record = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO *>(reinterpret_cast<const std::byte *>(record) + record->NextEntryOffset);
      }
    }
    const auto query_error = GetLastError();
    if (query_error != ERROR_NO_MORE_FILES) {
      fail(query_error);
    }
    return result;
  }

  download read(const std::filesystem::path &root, std::string_view relative, uint64_t offset, size_t limit, std::string_view version, const std::filesystem::path &private_directory) {
    const auto path = decode(relative);
    auto parts = components(path, L"/");
    if (parts.empty() || limit == 0 || limit > MAXDWORD) {
      throw error {error_code::invalid};
    }
    const auto filename = parts.back();
    parts.pop_back();
    const private_directory_guard private_scope {private_directory};
    const auto directories = open_directories(root, parts, 0, private_scope.get());
    const auto file = open_child(directories.back().get(), filename, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_OPEN, 0);
    auto metadata = verify_file(file.get());
    if (!version.empty() && metadata.version != version) {
      throw error {error_code::changed};
    }
    if (offset > metadata.size) {
      throw error {error_code::invalid};
    }
    const auto count = static_cast<DWORD>((std::min)(metadata.size - offset, static_cast<uint64_t>(limit)));
    std::string bytes(count, '\0');
    if (count != 0) {
      seek(file.get(), offset);
      DWORD received = 0;
      if (!ReadFile(file.get(), bytes.data(), count, &received, nullptr)) {
        fail(GetLastError());
      }
      if (received != count || verify_file(file.get()).version != metadata.version) {
        throw error {error_code::changed};
      }
    }
    return {std::move(bytes), metadata.size, std::move(metadata.version)};
  }

  /** @brief Directory handles outlive the exclusive staged file handle. */
  struct upload::impl {
    std::wstring destination;  ///< Validated leaf name for the commit.
    directory_chain directories;  ///< Pins the inbox and its ancestors.
    file_handle file;  ///< Exclusive, delete-on-close staged file.
    uint64_t size = 0;  ///< Number of successfully staged bytes.
  };

  upload::upload(const std::filesystem::path &inbox, std::string_view name, std::string_view id) {
    auto destination = decode(name);
    if (!valid_component(destination) || !valid_upload_id(id)) {
      throw error {error_code::invalid};
    }
    auto directories = open_directories(inbox, {}, FILE_ADD_FILE);
    const auto staging_name = std::wstring {stage_prefix} + decode(id);
    auto file = open_child(directories.back().get(), staging_name, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE);
    data = std::make_unique<impl>(std::move(destination), std::move(directories), std::move(file));
  }

  upload::~upload() = default;

  void upload::append(uint64_t offset, std::string_view content) {
    if (!data || offset != data->size || content.size() > MAXDWORD || content.size() > (std::numeric_limits<LONGLONG>::max)() - offset) {
      throw error {error_code::invalid};
    }
    if (content.empty()) {
      return;
    }
    const auto file = data->file.get();
    seek(file, offset);
    DWORD written = 0;
    const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    if (!success || written != content.size()) {
      const auto write_error = success ? ERROR_WRITE_FAULT : GetLastError();
      FILE_END_OF_FILE_INFO eof {};
      eof.EndOfFile.QuadPart = static_cast<LONGLONG>(offset);
      SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof));
      fail(write_error);
    }
    data->size += written;
  }

  void upload::commit() {
    if (!data) {
      throw error {error_code::invalid};
    }
    const auto file = data->file.get();
    if (!FlushFileBuffers(file)) {
      fail(GetLastError());
    }
    rename_upload(file, data->directories.back().get(), data->destination);

    // Only disarm automatic deletion after the complete file has been renamed.
    FILE_DISPOSITION_INFO_EX disposition {FILE_DISPOSITION_FLAG_ON_CLOSE};
    if (!SetFileInformationByHandle(file, FileDispositionInfoEx, &disposition, sizeof(disposition))) {
      fail(GetLastError());
    }
    data.reset();
  }
}  // namespace file_transfer::storage
