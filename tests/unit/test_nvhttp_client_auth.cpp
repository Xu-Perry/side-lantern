/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <type_traits>
#include <vector>

// lib includes
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/system/system_error.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/client_https.hpp>

// local includes
#include <src/config.h>
#include <src/manual_video_settings.h>
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/rtsp.h>

namespace fs = std::filesystem;

static_assert(std::is_same_v<decltype(&nvhttp::pin), bool (*)(std::string_view, const std::string &, std::string)>);

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  EXPECT_TRUE(nvhttp::test_support::add_client("invalid", "not a certificate", true).empty());
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, InterleavedHttpsRequestsUseTheirOwnPairedIdentity) {
  const auto server_credentials = crypto::gen_creds("Sunshine test host", 2048);
  const auto first_credentials = test_utils::certificates::generate_ca_credentials("Sunshine first client");
  const auto second_credentials = test_utils::certificates::generate_ca_credentials("Sunshine second client");
  const auto first_id = nvhttp::test_support::add_client("first", first_credentials.x509, true);
  const auto second_id = nvhttp::test_support::add_client("second", second_credentials.x509, true);
  ASSERT_FALSE(first_id.empty());
  ASSERT_FALSE(second_id.empty());

  const auto directory = fs::path {SUNSHINE_TEST_BIN_DIR} / "https_paired_client_identity";
  ASSERT_TRUE(fs::create_directories(directory) || fs::is_directory(directory));
  auto cleanup = util::fail_guard([&] {
    std::error_code error;
    fs::remove_all(directory, error);
  });
  std::ofstream {directory / "host.cert"} << server_credentials.x509;
  std::ofstream {directory / "host.key"} << server_credentials.pkey;
  std::ofstream {directory / "first.cert"} << first_credentials.x509;
  std::ofstream {directory / "first.key"} << first_credentials.pkey;
  std::ofstream {directory / "second.cert"} << second_credentials.x509;
  std::ofstream {directory / "second.key"} << second_credentials.pkey;

  std::shared_ptr<SimpleWeb::ScopeRunner> scope;
  fs::path file_transfer_settings;
#ifdef _WIN32
  file_transfer_settings = directory / "file-transfer.json";
  std::ofstream {file_transfer_settings} << nlohmann::json {
    {"inbox", directory.generic_string()}, {"devices", nlohmann::json::array({first_id, second_id})}
  };
#endif
  auto server = nvhttp::test_support::make_https_server(
    (directory / "host.cert").string(), (directory / "host.key").string(), scope, true, file_transfer_settings
  );
  server->io_service = std::make_shared<boost::asio::io_context>();
  server->config.address = "127.0.0.1";
  server->config.port = 0;
  unsigned short port = 0;
  server->start([&](const unsigned short assigned) { port = assigned; });
  // With an external io_context, the listener's readiness callback runs when the test polls it.
  ASSERT_EQ(server->io_service->poll_one(), 1);
  ASSERT_NE(port, 0);
  std::jthread worker {[&] { server->io_service->run(); }};
  auto stop = util::fail_guard([&] { server->stop(); });

  const auto address = "127.0.0.1:" + std::to_string(port);
  SimpleWeb::Client<SimpleWeb::HTTPS> first {address, false, (directory / "first.cert").string(), (directory / "first.key").string()};
  SimpleWeb::Client<SimpleWeb::HTTPS> second {address, false, (directory / "second.cert").string(), (directory / "second.key").string()};
  first.config.timeout = first.config.timeout_connect = 2;
  second.config.timeout = second.config.timeout_connect = 2;
  EXPECT_EQ(first.request("GET", "/test-verified-client")->content.string(), first_id);
#ifdef _WIN32
  constexpr auto upload_path = "/sidelantern/v1/files/uploads";
  const auto files = first.request("GET", "/sidelantern/v1/files");
  ASSERT_EQ(files->status_code, "200 OK");
  const auto file_capabilities = nlohmann::json::parse(files->content.string());
  EXPECT_EQ(file_capabilities.at("upload"), true);
  EXPECT_FALSE(file_capabilities.at("roots").empty());
  EXPECT_EQ(file_capabilities.at("roots").front().at("kind"), "drive");
  const auto file_name = "tls-test-" + first_id + ".bin";
  const auto upload = first.request("POST", upload_path, nlohmann::json {{"name", file_name}, {"size", 2}}.dump(),
                                    {{"Content-Type", "application/json"}});
  ASSERT_EQ(upload->status_code, "201 Created");
  const auto id = nlohmann::json::parse(upload->content.string()).at("id").get<std::string>();
  const auto owned_path = std::string(upload_path) + "?id=" + id;
  EXPECT_EQ(second.request("GET", owned_path)->status_code, "404 Not Found");
  const auto bytes = std::string("x\0", 2);
  EXPECT_EQ(first.request("PUT", owned_path + "&offset=0", bytes, {{"Content-Type", "application/octet-stream"}})->status_code, "200 OK");
  EXPECT_EQ(first.request("POST", std::string(upload_path) + "/complete?id=" + id)->status_code, "200 OK");
  std::ifstream file {directory / file_name, std::ios::binary};
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), bytes);
#endif
  EXPECT_EQ(second.request("GET", "/test-verified-client")->content.string(), second_id);
  EXPECT_EQ(first.request("GET", "/test-verified-client")->content.string(), first_id);
  {
    SimpleWeb::Client<SimpleWeb::HTTPS> upload {address, false, (directory / "second.cert").string(), (directory / "second.key").string()};
    upload.config.timeout = upload.config.timeout_connect = 2;
    // An oversized Content-Length must be rejected before the client sends its body.
    const SimpleWeb::CaseInsensitiveMultimap headers {{"Content-Length", "65537"}, {"Expect", "100-continue"}};
    try {
      const auto oversized = upload.request("POST", "/sidelantern/v1/manual-video", std::string {}, headers);
      EXPECT_TRUE(oversized->status_code.starts_with("413 "));
    } catch (const boost::system::system_error &error) {
      // The HTTP library can close the socket before flushing the oversize response.
      EXPECT_TRUE(error.code() == boost::asio::error::eof || error.code() == boost::asio::ssl::error::stream_truncated) << error.what();
    }
  }
  ASSERT_TRUE(nvhttp::unpair_client(first_id));
#ifdef _WIN32
  EXPECT_EQ(first.request("GET", "/sidelantern/v1/files")->status_code, "401 Unauthorized");
#endif
  EXPECT_EQ(first.request("GET", "/test-verified-client")->status_code, "401 Unauthorized");
  EXPECT_EQ(second.request("GET", "/test-verified-client")->content.string(), second_id);

  // GameStream XML endpoints close the TLS connection after each response.
  const auto request_mode = [&](const std::string &path, const std::string &peer = "second", const std::string &method = "GET") {
    SimpleWeb::Client<SimpleWeb::HTTPS> client {address, false, (directory / (peer + ".cert")).string(), (directory / (peer + ".key")).string()};
    client.config.timeout = client.config.timeout_connect = 2;
    const auto response = client.request(method, path);
    return std::pair {response->status_code, response->content.string()};
  };
  const auto modes = request_mode("/sidelantern/v1/stream-modes").second;
  EXPECT_NE(modes.find("version=\"1\""), std::string::npos);
  EXPECT_NE(modes.find("join_current=\"0\""), std::string::npos);
  for (const auto *id : {"primary", "secondary", "extend"}) {
    EXPECT_NE(modes.find(std::string {"id=\""} + id + '"'), std::string::npos);
  }
  for (const auto *path : {"/launch?sidelantern_mode=invalid", "/resume?sidelantern_mode=invalid"}) {
    const auto rejected = request_mode(path).second;
    EXPECT_NE(rejected.find("status_code=\"400\""), std::string::npos);
    EXPECT_NE(rejected.find("unknown_mode"), std::string::npos);
  }
  {
    auto layout = std::make_shared<platf::deinit_t>();
    const std::weak_ptr<platf::deinit_t> lifetime = layout;
    auto active = rtsp_stream::test_support::insert_session(first_id, "Virtual display client", true, {}, {"virtual", std::move(layout)});
    auto clear = util::fail_guard([&] { rtsp_stream::test_support::remove_session(active); });
    const auto playing = request_mode("/sidelantern/v1/stream-modes").second;
    EXPECT_NE(playing.find("join_current=\"1\""), std::string::npos);
    for (const auto *id : {"primary", "secondary", "extend"}) {
      EXPECT_NE(playing.find(std::string {"id=\""} + id + "\" available=\"1\""), std::string::npos);
    }
    const auto incomplete = request_mode("/launch?sidelantern_mode=extend").second;
    EXPECT_NE(incomplete.find("Missing a required launch parameter"), std::string::npos);
    const auto sessions = rtsp_stream::active_sessions();
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions.front().device_id, first_id);
    const auto joined = request_mode("/launch?sidelantern_mode=primary&appid=0&localAudioPlayMode=1&rikeyid=1&rikey=46464646464646464646464646464646").second;
    auto ticket = rtsp_stream::test_support::pending_launch();
    ASSERT_TRUE(ticket) << joined;
    const auto ticket_id = ticket->id;
    auto clear_ticket = util::fail_guard([ticket_id] { rtsp_stream::launch_session_clear(ticket_id); });
    EXPECT_NE(joined.find("status_code=\"200\""), std::string::npos);
    EXPECT_EQ(ticket->required_display_id, "virtual");
    EXPECT_EQ(ticket->display_claim, lifetime.lock());
    clear.disable();
    rtsp_stream::test_support::remove_session(active);
    active.reset();
    ticket.reset();
    EXPECT_FALSE(lifetime.expired());
    rtsp_stream::launch_session_clear(ticket_id);
    clear_ticket.disable();
    EXPECT_TRUE(lifetime.expired());
  }
  EXPECT_EQ(request_mode("/sidelantern/v1/stream-modes", "first").first, "401 Unauthorized");
  const std::string manual_path = "/sidelantern/v1/manual-video";
  EXPECT_NE(request_mode(manual_path).second.find("no_manual_video_session"), std::string::npos);
  const auto settings = std::make_shared<video::manual_video_settings_t>(video::video_settings_t {1920, 1080, 60, 12000});
  {
    auto other = rtsp_stream::test_support::insert_session(first_id, "Another client", true, settings);
    auto clear = util::fail_guard([&] { rtsp_stream::test_support::remove_session(other); });
    EXPECT_NE(request_mode(manual_path).second.find("no_manual_video_session"), std::string::npos);
  }
  {
    auto active = rtsp_stream::test_support::insert_session(second_id, "Manual client", true, settings);
    auto clear = util::fail_guard([&] { rtsp_stream::test_support::remove_session(active); });
    const auto args = "&width=1280&height=720&fps=30&bitrate_kbps=7000";
    EXPECT_EQ(request_mode(manual_path, "first").first, "401 Unauthorized");
    EXPECT_NE(request_mode(manual_path + "?session_id=41" + args, "second", "POST").second.find("stale_session"), std::string::npos);
    EXPECT_NE(request_mode(manual_path + "?session_id=42" + args + "&fps=6", "second", "POST").second.find("invalid_video_settings"), std::string::npos);
    EXPECT_NE(request_mode(manual_path + "?session_id=42" + args, "second", "POST").second.find("status_code=\"202\""), std::string::npos);
    EXPECT_NE(request_mode(manual_path).second.find("<status>pending</status>"), std::string::npos);
    settings->finish(true);
    const auto applied = request_mode(manual_path).second;
    EXPECT_NE(applied.find("<width>1280</width>"), std::string::npos);
    EXPECT_NE(applied.find("<fps>30</fps>"), std::string::npos);
    EXPECT_NE(applied.find("<bitrate_kbps>7000</bitrate_kbps>"), std::string::npos);
  }
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
  EXPECT_TRUE(nvhttp::is_authorized_stream_client(enabled_uuid, enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::is_authorized_stream_client(expired_uuid, enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::is_authorized_stream_client(disabled_uuid, disabled_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::is_authorized_stream_client(enabled_uuid, enabled_credentials.x509));
  EXPECT_TRUE(nvhttp::is_authorized_stream_client(expired_uuid, expired_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_FALSE(nvhttp::test_support::duplicate_client("missing"));
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RePairingReplacesDuplicateCertificateIdentity) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto original_uuid = nvhttp::test_support::add_client("original", paired_credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_FALSE(original_uuid.empty());
  ASSERT_FALSE(other_uuid.empty());
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(original_uuid));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));

  const auto repaired_uuid = nvhttp::test_support::add_client(
    "repaired",
    test_utils::certificates::to_crlf_pem(paired_credentials.x509),
    true
  );

  ASSERT_FALSE(repaired_uuid.empty());
  EXPECT_NE(repaired_uuid, original_uuid);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_EQ(nvhttp::get_cert_by_uuid(repaired_uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}
