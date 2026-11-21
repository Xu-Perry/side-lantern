/**
 * @file tests/unit/test_file_transfer.cpp
 * @brief Tests for the SideLantern file transfer protocol.
 */
#ifdef _WIN32
// test includes
#include "tests/tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

// lib includes
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <shlobj.h>

// local includes
#include "src/file_transfer.h"
#include "src/utility.h"
#include "src/uuid.h"

namespace {
  using file_transfer::operation;
  namespace fs = std::filesystem;
  using json = nlohmann::json;
  using namespace std::literals;

  /** @brief Isolated on-disk share and inbox for protocol tests. */
  struct fixture {
    fs::path folder = fs::temp_directory_path() / ("file-transfer-" + uuid_util::uuid_t::generate().string());
    fs::path settings = folder / "file-transfer.json";
    fs::path private_directory = folder / "private";
    file_transfer::service server {settings, private_directory};

    /** @brief Set up a clean test filesystem. */
    fixture() {
      fs::create_directories(folder / "share");
      fs::create_directories(folder / "inbox");
      fs::create_directories(private_directory);
    }

    /** @brief Remove the test filesystem, after the test has closed its upload files. */
    ~fixture() {
      server.revoke("allowed");
      server.revoke("other");
      std::error_code ec;
      fs::remove_all(folder, ec);
    }

    /** @brief Allow the selected test clients and attach the test folders. */
    void configure(json devices = json::array({ "allowed", "other" })) {
      std::ofstream {settings} << json {
        {"devices", devices},
        {"inbox", (folder / "inbox").generic_string()},
        {"roots", json::array({ {{"id", "docs"}, {"label", "Shared"}, {"path", (folder / "share").generic_string()}} })}
      };
    }
  };
}  // namespace

TEST(FileTransfer, BrowsesLocalDrivesWithoutAShareConfiguration) {
  fixture test;
  const auto capabilities = test.server.execute("allowed", operation::capabilities);
  ASSERT_EQ(capabilities.status, 200);
  const auto info = json::parse(capabilities.body);
  EXPECT_EQ(info.at("enabled"), true);
  const auto root = test.folder.root_name().string();
  const auto id = "drive-" + root.substr(0, 1);
  bool found = false;
  for (const auto &drive : info.at("roots")) {
    if (drive.at("id") == id) {
      found = true;
      EXPECT_EQ(drive.at("kind"), "drive");
      EXPECT_EQ(drive.at("label"), root);
      EXPECT_FALSE(drive.contains("path"));
    }
  }
  ASSERT_TRUE(found);
  std::ofstream {test.folder / "share" / "direct.txt"} << "from the drive";
  std::ofstream {test.private_directory / "key.txt"} << "protected";
  const auto relative = test.folder.relative_path().generic_string();
  const auto contents = test.server.execute("allowed", operation::list, {{"root", id}, {"path", relative}});
  ASSERT_EQ(contents.status, 200) << contents.body;
  const auto entries = json::parse(contents.body).at("entries");
  for (const auto &entry : entries) {
    EXPECT_NE(entry.at("name"), "private");
  }
  EXPECT_EQ(test.server.execute("allowed", operation::list, {{"root", id}, {"path", relative + "/private"}}).status, 403);
  EXPECT_EQ(test.server.execute("allowed", operation::download, {{"root", id}, {"path", relative + "/private/key.txt"}}).status, 403);
  const auto download = test.server.execute("allowed", operation::download, {{"root", id}, {"path", relative + "/share/direct.txt"}});
  ASSERT_EQ(download.status, 200) << download.body;
  EXPECT_EQ(download.body, "from the drive");
}

TEST(FileTransfer, ReceivesFilesInWindowsDownloadsWithoutAnInboxSetting) {
  const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ASSERT_TRUE(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE);
  const auto uninitialize = util::fail_guard([com] {
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
  });
  PWSTR raw = nullptr;
  const auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &raw);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> downloads {raw, CoTaskMemFree};
  if (FAILED(result)) {
    GTEST_SKIP() << "This account has no accessible Windows Downloads folder";
  }
  ASSERT_NE(downloads, nullptr);

  fixture test;
  const auto name = "side-lantern-test-" + uuid_util::uuid_t::generate().string() + ".txt";
  const auto destination = fs::path {downloads.get()} / name;
  ASSERT_FALSE(fs::exists(destination));
  const auto cleanup = util::fail_guard([&] {
    std::error_code ec;
    fs::remove(destination, ec);
  });
  const auto capabilities = test.server.execute("allowed", operation::capabilities);
  ASSERT_EQ(capabilities.status, 200);
  EXPECT_EQ(json::parse(capabilities.body).at("upload"), true);
  const auto request = json {{"name", name}, {"size", 3}}.dump();
  const auto created = test.server.execute("allowed", operation::create_upload, {}, "application/json", request);
  ASSERT_EQ(created.status, 201) << created.body;
  const auto id = json::parse(created.body).at("id").get<std::string>();
  std::ofstream {test.settings} << "{}";
  ASSERT_EQ(test.server.execute("allowed", operation::append_upload, {{"id", id}, {"offset", "0"}}, "application/octet-stream", "abc").status, 200);
  ASSERT_EQ(test.server.execute("allowed", operation::complete_upload, {{"id", id}}).status, 200);
  std::ifstream file {destination, std::ios::binary};
  ASSERT_TRUE(file);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "abc");
}

TEST(FileTransfer, PreservesExplicitLegacyRootRestrictions) {
  fixture test;
  test.configure(json::array({"allowed"}));
  EXPECT_EQ(test.server.execute("other", operation::capabilities).status, 403);
  const auto capabilities = test.server.execute("allowed", operation::capabilities);
  ASSERT_EQ(capabilities.status, 200);
  const auto roots = json::parse(capabilities.body).at("roots");
  ASSERT_EQ(roots.size(), 1u);
  EXPECT_EQ(roots[0].at("id"), "docs");
  const auto drive_id = "drive-" + test.folder.root_name().string().substr(0, 1);
  EXPECT_EQ(test.server.execute("allowed", operation::list, {{"root", drive_id}}).status, 404);
}

TEST(FileTransfer, ListsSharedFilesAndReadsBinaryChunks) {
  fixture test;
  test.configure();
  std::ofstream(test.folder / "share" / "sample.bin", std::ios::binary).write("a\0bc", 4);
  const auto roots = test.server.execute("allowed", operation::capabilities);
  EXPECT_EQ(json::parse(roots.body).at("roots")[0], (json {{"id", "docs"}, {"label", "Shared"}, {"kind", "share"}}));
  const auto listing = test.server.execute("allowed", operation::list, {{"root", "docs"}});
  ASSERT_EQ(listing.status, 200);
  EXPECT_EQ(json::parse(listing.body).at("entries")[0].at("name"), "sample.bin");
  const auto chunk = test.server.execute("allowed", operation::download, {{"root", "docs"}, {"path", "sample.bin"}, {"length", "2"}});
  ASSERT_EQ(chunk.status, 200);
  EXPECT_EQ(chunk.body, "a\0"sv);
  EXPECT_EQ(chunk.headers.at("Content-Type"), "application/octet-stream");
  EXPECT_EQ(chunk.headers.at("X-SideLantern-File-Size"), "4");
  const auto version = chunk.headers.at("X-SideLantern-File-Version");
  const auto rest = test.server.execute("allowed", operation::download, {
    {"root", "docs"}, {"path", "sample.bin"}, {"offset", "2"}, {"version", version}
  });
  EXPECT_EQ(rest.body, "bc");
  EXPECT_EQ(test.server.execute("allowed", operation::download, {
    {"root", "docs"}, {"path", "sample.bin"}, {"offset", "2"}
  }).status, 400);
  EXPECT_EQ(test.server.execute("allowed", operation::list, {{"root", "docs"}, {"root", "other"}}).status, 400);
}

TEST(FileTransfer, UploadsRequireOwnerAndOnlyPublishCompleteFiles) {
  fixture test;
  test.configure();
  auto &server = test.server;
  const auto created = server.execute("allowed", operation::create_upload, {}, "application/json", R"({"name":"output.bin","size":4})");
  ASSERT_EQ(created.status, 201) << created.body;
  const auto id = json::parse(created.body).at("id").get<std::string>();
  const file_transfer::query_t upload {{"id", id}};
  const file_transfer::query_t first {{"id", id}, {"offset", "0"}};
  const file_transfer::query_t last {{"id", id}, {"offset", "2"}};
  EXPECT_EQ(server.execute("other", operation::get_upload, upload).status, 404);
  EXPECT_EQ(server.execute("allowed", operation::complete_upload, upload).status, 409);
  EXPECT_EQ(server.execute("allowed", operation::append_upload, last, "application/octet-stream", "bc").status, 409);
  EXPECT_EQ(server.execute("allowed", operation::append_upload, first, "application/octet-stream", "a\0"sv).status, 200);
  EXPECT_EQ(server.execute("allowed", operation::append_upload, first, "application/octet-stream", "a\0"sv).status, 409);
  EXPECT_FALSE(fs::exists(test.folder / "inbox" / "output.bin"));
  EXPECT_EQ(server.execute("allowed", operation::append_upload, last, "application/octet-stream", "bc").status, 200);
  EXPECT_EQ(server.execute("allowed", operation::complete_upload, upload).status, 200);
  EXPECT_EQ(server.execute("allowed", operation::complete_upload, upload).status, 200);
  std::ifstream file {test.folder / "inbox" / "output.bin", std::ios::binary};
  ASSERT_TRUE(file);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "a\0bc"sv);
}

TEST(FileTransfer, CancelsUploadsAndDropsRevokedAccess) {
  fixture test;
  test.configure();
  const auto start = [&test] {
    return test.server.execute("allowed", operation::create_upload, {}, "application/json", R"({"name":"output.bin","size":4})");
  };
  auto result = start();
  ASSERT_EQ(result.status, 201);
  auto id = json::parse(result.body).at("id").get<std::string>();
  EXPECT_EQ(test.server.execute("allowed", operation::cancel_upload, {{"id", id}}).status, 200);
  EXPECT_EQ(test.server.execute("allowed", operation::get_upload, {{"id", id}}).status, 404);
  result = start();
  ASSERT_EQ(result.status, 201);
  id = json::parse(result.body).at("id").get<std::string>();
  test.configure(json::array({"other"}));
  EXPECT_EQ(test.server.execute("allowed", operation::get_upload, {{"id", id}}).status, 403);
  test.configure();
  EXPECT_EQ(test.server.execute("allowed", operation::get_upload, {{"id", id}}).status, 404);
  EXPECT_TRUE(fs::is_empty(test.folder / "inbox"));
}

TEST(FileTransfer, EnforcesUploadLimitsAndSupportsEmptyUploads) {
  fixture test;
  test.configure();
  auto &server = test.server;
  const auto start = [&](std::string_view body) {
    return server.execute("allowed", operation::create_upload, {}, "application/json", body);
  };
  EXPECT_EQ(start(R"({"name":"bad.bin","size":-1})").status, 400);
  EXPECT_EQ(start(R"({"name":"huge.bin","size":8589934593})").status, 413);
  EXPECT_EQ(server.execute("allowed", operation::create_upload, {}, "text/plain", "{}").status, 415);
  const auto created = start(R"({"name":"empty.bin","size":0})");
  ASSERT_EQ(created.status, 201);
  const auto id = json::parse(created.body).at("id").get<std::string>();
  EXPECT_EQ(server.execute("allowed", operation::complete_upload, {{"id", id}}).status, 200);
  EXPECT_EQ(fs::file_size(test.folder / "inbox" / "empty.bin"), 0);
}
#endif
