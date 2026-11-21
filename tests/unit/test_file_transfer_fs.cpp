/**
 * @file tests/unit/test_file_transfer_fs.cpp
 * @brief Tests for file-transfer storage on Windows.
 */
#ifdef _WIN32

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

// lib includes
#include <Windows.h>
#include <winioctl.h>

// local includes
#include "src/platform/windows/file_transfer_fs.h"

namespace {
  namespace fs = std::filesystem;
  namespace storage = file_transfer::storage;

  constexpr auto upload_id = "01234567-89ab-4cde-8f01-23456789abcd";

  /**
   * @brief Own a temporary local directory with a separate share and outside directory.
   */
  class FileTransferFsTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      std::random_device random;
      auto staged = fs::temp_directory_path() / (L"sidelantern-fs-test-" + std::to_wstring(random()) + L"-" + std::to_wstring(random()));
      ASSERT_TRUE(fs::create_directory(staged));
      base = std::move(staged);
      ASSERT_TRUE(fs::create_directory(share()));
      ASSERT_TRUE(fs::create_directory(base / L"outside"));
    }

    void TearDown() override {
      std::error_code ignored;
      fs::remove_all(base, ignored);
      BaseTest::TearDown();
    }

    /** @brief Return the directory authorized for file transfer. */
    fs::path share() const {
      return base / L"share";
    }

    /** @brief Put bytes in a temporary file. */
    void store(const fs::path &path, std::string_view bytes) const {
      std::ofstream output {path, std::ios::binary | std::ios::trunc};
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      ASSERT_TRUE(output.good());
    }

    fs::path base;  ///< Owns the share and files outside it.
  };

  /**
   * @brief Expect a storage operation to fail with its public error code.
   */
  template<class F>
  void expect_error(storage::error_code expected, F &&action) {
    try {
      action();
      ADD_FAILURE() << "Expected storage error " << static_cast<int>(expected);
    } catch (const storage::error &failure) {
      EXPECT_EQ(failure.code, expected);
    }
  }

  /**
   * @brief Make a junction that can be created without symbolic-link privilege.
   */
  bool create_junction(const fs::path &link, const fs::path &target) {
    if (!fs::create_directory(link)) {
      return false;
    }
    const std::wstring substitute = L"\\??\\" + target.native();
    struct {
      DWORD tag;
      WORD data_length;
      WORD reserved;
      WORD substitute_offset;
      WORD substitute_length;
      WORD print_offset;
      WORD print_length;
      wchar_t paths[MAX_PATH * 2];
    } data {};
    if (substitute.size() + target.native().size() + 2 > std::size(data.paths)) {
      return false;
    }
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.native().size() * sizeof(wchar_t));
    data.data_length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    substitute.copy(data.paths, substitute.size());
    target.native().copy(data.paths + data.print_offset / sizeof(wchar_t), target.native().size());
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }
    DWORD bytes = 0;
    const bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.data_length + 8, nullptr, 0, &bytes, nullptr);
    CloseHandle(handle);
    return result;
  }

  TEST_F(FileTransferFsTest, ListsBoundedPagesWithReusableFileVersions) {
    ASSERT_TRUE(fs::create_directory(share() / L"nested"));
    store(share() / L"notes.txt", "abcde");
    store(share() / L"nested" / L"nested.txt", "nested");

    const auto first = storage::list(share(), "", 0, 1);
    ASSERT_EQ(first.entries.size(), 1);
    ASSERT_EQ(first.next_offset, 1);
    const auto second = storage::list(share(), "", *first.next_offset, 1);
    ASSERT_EQ(second.entries.size(), 1);
    EXPECT_EQ(second.next_offset, std::nullopt);

    const auto &directory = first.entries.front().directory ? first.entries.front() : second.entries.front();
    EXPECT_TRUE(directory.directory);
    EXPECT_EQ(directory.name, "nested");
    const auto &file = first.entries.front().directory ? second.entries.front() : first.entries.front();
    EXPECT_FALSE(file.directory);
    EXPECT_EQ(file.name, "notes.txt");
    EXPECT_EQ(file.size, 5);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    EXPECT_NEAR(file.modified_unix_ms, now, 10000);
    ASSERT_FALSE(file.version.empty());
    EXPECT_EQ(storage::read(share(), file.name, 0, 5, file.version).bytes, "abcde");
    const auto nested = storage::list(share(), "nested", 0, 1);
    ASSERT_EQ(nested.entries.size(), 1);
    EXPECT_EQ(nested.entries.front().name, "nested.txt");
  }

  TEST_F(FileTransferFsTest, ReadsBinaryChunksAndEmptyFiles) {
    store(share() / L"bin", std::string_view("a\0bc", 4));
    store(share() / L"empty", "");

    const auto first = storage::read(share(), "bin", 0, 2, "");
    ASSERT_EQ(first.bytes, std::string("a\0", 2));
    EXPECT_EQ(first.size, 4);
    const auto last = storage::read(share(), "bin", 2, 100, first.version);
    EXPECT_EQ(last.bytes, "bc");
    EXPECT_EQ(last.version, first.version);
    const auto empty = storage::read(share(), "empty", 0, 1, "");
    EXPECT_TRUE(empty.bytes.empty());
    EXPECT_EQ(empty.size, 0);
    EXPECT_FALSE(empty.version.empty());
  }

  TEST_F(FileTransferFsTest, RejectsAFileThatChangedBetweenChunks) {
    store(share() / L"change", "first");
    const auto first = storage::read(share(), "change", 0, 2, "");
    store(share() / L"change", "updated");
    expect_error(storage::error_code::changed, [&] { storage::read(share(), "change", 2, 2, first.version); });
    expect_error(storage::error_code::invalid, [&] { storage::read(share(), "change", 8, 2, ""); });
  }

  TEST_F(FileTransferFsTest, RoundTripsUnicodeFilenames) {
    const auto name = "\xe6\x96\x87\xe4\xbb\xb6-\xf0\x9f\x93\x84.txt";
    store(share() / L"\u6587\u4ef6-\U0001f4c4.txt", "text");
    const auto listing = storage::list(share(), "", 0, 1);
    ASSERT_EQ(listing.entries.size(), 1);
    EXPECT_EQ(listing.entries.front().name, name);
    EXPECT_EQ(storage::read(share(), name, 0, 10, "").bytes, "text");
  }

  TEST_F(FileTransferFsTest, RejectsUnsafeComponentsAndDrivePrefixes) {
    for (const std::string_view relative : {
           "../outside/secret", "./file", "/file", "dir//file", "dir/", "dir\\file", "file::$DATA", "file.", "file ", "file\x7f", "a?b", "aux.txt", "Con .txt", "com\xc2\xb9.txt", "CONIN$", ".SideLantern-upload-token", "\xed\xa0\x80"
         }) {
      SCOPED_TRACE(relative);
      expect_error(storage::error_code::invalid, [&] { storage::read(share(), relative, 0, 1, ""); });
    }
    for (const fs::path root : {L"relative", L"C:drive-relative", L"\\\\outside-host\\share", L"\\\\?\\C:\\"}) {
      expect_error(storage::error_code::invalid, [&] { storage::list(root, "", 0, 1); });
    }
    expect_error(storage::error_code::invalid, [&] { storage::list(share() / L"..", "", 0, 1); });
    expect_error(storage::error_code::not_found, [&] { storage::list(base / L"removed-share", "", 0, 1); });
  }

  TEST_F(FileTransferFsTest, RejectsJunctionsInsideAndAtTheShareBoundary) {
    store(base / L"outside" / L"secret", "secret");
    const auto junction = share() / L"junction";
    ASSERT_TRUE(create_junction(junction, base / L"outside")) << GetLastError();

    EXPECT_TRUE(storage::list(share(), "", 0, 10).entries.empty());
    expect_error(storage::error_code::forbidden, [&] { storage::read(share(), "junction/secret", 0, 100, ""); });
    expect_error(storage::error_code::forbidden, [&] { storage::list(junction, "", 0, 10); });
    expect_error(storage::error_code::forbidden, [&] { storage::upload(junction, "arriving.txt", upload_id); });
  }

  TEST_F(FileTransferFsTest, RejectsHardLinkedFiles) {
    const auto original = base / L"outside" / L"secret";
    const auto link = share() / L"appears-local";
    store(original, "secret");
    ASSERT_TRUE(CreateHardLinkW(link.c_str(), original.c_str(), nullptr)) << GetLastError();
    expect_error(storage::error_code::forbidden, [&] { storage::read(share(), "appears-local", 0, 100, ""); });
  }

  TEST_F(FileTransferFsTest, PublishesAnUploadOnlyOnCommit) {
    storage::upload incoming {share(), "arriving.txt", upload_id};
    incoming.append(0, "first");
    incoming.append(5, " second");
    EXPECT_FALSE(fs::exists(share() / L"arriving.txt"));
    EXPECT_TRUE(storage::list(share(), "", 0, 10).entries.empty());
    incoming.commit();
    EXPECT_EQ(storage::read(share(), "arriving.txt", 0, 50, "").bytes, "first second");
  }

  TEST_F(FileTransferFsTest, CanceledAndConflictingUploadsLeaveNoArtifacts) {
    {
      storage::upload abandoned {share(), "abandoned.txt", upload_id};
      abandoned.append(0, "partial");
      expect_error(storage::error_code::invalid, [&] { abandoned.append(1, "wrong offset"); });
    }
    EXPECT_TRUE(fs::is_empty(share()));
    store(share() / L"existing.txt", "keep me");
    {
      storage::upload conflict {share(), "existing.txt", upload_id};
      conflict.append(0, "please replace me");
      expect_error(storage::error_code::conflict, [&] { conflict.commit(); });
    }
    EXPECT_EQ(std::distance(fs::directory_iterator(share()), fs::directory_iterator {}), 1);
    EXPECT_EQ(storage::read(share(), "existing.txt", 0, 100, "").bytes, "keep me");
  }
}  // namespace

#endif
