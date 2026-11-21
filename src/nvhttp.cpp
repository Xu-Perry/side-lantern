/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#ifdef _WIN32
  #include "file_transfer.h"
#endif
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "manual_video_settings.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "string_hash.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;
  static constexpr std::size_t GAMESTREAM_REQUEST_BUFFER_SIZE = 64 * 1024;  ///< Bound the buffered HTTP headers and control request bodies.

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;  ///< Enabled paired-client certificates accepted by Sunshine's GameStream HTTPS server.

  /**
   * @brief Get the mutex protecting paired-client authorization state.
   *
   * @return Mutex serializing paired-client state and certificate authorization changes.
   */
  std::mutex &client_auth_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  /**
   * @brief HTTPS server backend that adds Sunshine's client-certificate verification.
   */
  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    /**
     * @brief Initialize the HTTPS server with Sunshine's certificate and key files.
     *
     * @param certification_file Path to the server certificate file.
     * @param private_key_file Path to the matching private key file.
     */
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443) {
      config.max_request_streambuf_size = GAMESTREAM_REQUEST_BUFFER_SIZE;
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<std::optional<std::string>(SSL *)> verify;  ///< Callback returning the verified client's certificate after handshake.
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;  ///< Handler used to return the pairing challenge when client verification fails.

    /**
     * @brief Retrieve the verified certificate for the exact transport connection serving this request.
     * @param request HTTPS request containing the connection endpoints.
     * @return Canonical PEM certificate, or an empty string when the connection has no verified identity.
     */
    std::string verified_certificate(const Request &request) {
      std::lock_guard lock {verified_connections_mutex};
      const auto identity = verified_connections.find({request.local_endpoint(), request.remote_endpoint()});
      if (identity == verified_connections.end() || identity->second.connection.expired()) {
        return {};
      }
      return identity->second.certificate;
    }

  protected:
    boost::asio::ssl::context context {boost::asio::ssl::context::tls_server};  ///< TLS server context configured with Sunshine's certificate and protocol policy.

    /**
     * @brief Enable client-certificate verification after the listening socket is bound.
     * @details Ignore the TLS preverification result and context so failed authorization can receive an HTTP error.
     */
    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int, boost::asio::ssl::verify_context &) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    /**
     * @brief Process a completed TLS handshake under the callback's server lifetime guard.
     *
     * @param session Session whose TLS handshake has completed.
     * @param ec Result of the asynchronous handshake.
     * @pre The caller holds the session's handler scope guard.
     */
    void handle_handshake(const std::shared_ptr<Session> &session, const SimpleWeb::error_code &ec) {
      if (!ec) {
        if (verify) {
          auto certificate = verify(session->connection->socket->native_handle());
          if (!certificate || !remember_verified_connection(session, std::move(*certificate))) {
            this->write(session, on_verify_failed);
            return;
          }
        }
        this->read(session);
      } else if (this->on_error) {
        this->on_error(session->request, ec);
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    /**
     * @brief Accept a pending connection and arm the server for the next client.
     * @details Keep each handler's scope guard alive until all access to the server has finished.
     */
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        if (auto lock = connection->handler_runner->continue_lock(); lock) {
          if (ec != SimpleWeb::error::operation_aborted) {
            this->accept();
          }

          auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

          if (!ec) {
            boost::asio::ip::tcp::no_delay option(true);
            SimpleWeb::error_code option_error;
            session->connection->socket->lowest_layer().set_option(option, option_error);

            session->connection->set_timeout(config.timeout_request);
            session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
              session->connection->cancel_timeout();
              if (auto lock = session->connection->handler_runner->continue_lock(); lock) {
                this->handle_handshake(session, ec);
              }
            });
          } else if (this->on_error) {
            this->on_error(session->request, ec);
          }
        }
      });
    }

  private:
    using endpoint_t = boost::asio::ip::tcp::endpoint;  ///< TCP endpoint of a verified TLS connection.

    /**
     * @brief Certificate identity kept only for the lifetime of a TLS transport connection.
     */
    struct verified_connection_t {
      std::weak_ptr<Connection> connection;  ///< Underlying connection observed without extending its lifetime.
      std::string certificate;  ///< Canonical PEM certificate of the verified peer.
    };

    /**
     * @brief Publish a certificate identity before reading requests on a verified transport.
     * @param session The session whose TLS handshake succeeded.
     * @param certificate Canonical PEM certificate of the verified peer.
     * @return Whether the verified TLS peer could be associated with its connection endpoints.
     */
    bool remember_verified_connection(const std::shared_ptr<Session> &session, std::string certificate) {
      SimpleWeb::error_code error;
      const auto &socket = session->connection->socket->lowest_layer();
      const auto local = socket.local_endpoint(error);
      if (error) {
        return false;
      }
      const auto remote = socket.remote_endpoint(error);
      if (error) {
        return false;
      }

      std::lock_guard lock {verified_connections_mutex};
      std::erase_if(verified_connections, [](const auto &entry) { return entry.second.connection.expired(); });
      verified_connections.insert_or_assign({local, remote}, verified_connection_t {session->connection, std::move(certificate)});
      return true;
    }

    std::mutex verified_connections_mutex;  ///< Guards the per-transport identity registry.
    std::map<std::pair<endpoint_t, endpoint_t>, verified_connection_t> verified_connections;  ///< Verified identities indexed by local and remote TCP endpoints.
  };

  /**
   * @brief HTTPS server type used for GameStream endpoints requiring TLS.
   */
  using https_server_t = SunshineHTTPSServer;
  /**
   * @brief Plain HTTP server type used for GameStream endpoints without TLS.
   */
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  /**
   * @brief Internal HTTPS credential paths for the configuration server.
   */
  struct conf_intern_t {
    std::string servercert;  ///< Server certificate PEM string.
    std::string pkey;  ///< Private key PEM string or path.
  };

  conf_intern_t conf_intern;  ///< TLS credential paths loaded from Sunshine's runtime configuration.

  /**
   * @brief Certificate entry associated with a client name and UUID.
   */
  struct named_cert_t {
    std::string name;  ///< Human-readable name for this item.
    std::string uuid;  ///< Persistent Moonlight client UUID associated with the certificate.
    std::string cert;  ///< Certificate PEM string or path.
    bool enabled = true;  ///< Whether this persisted client entry may connect.
  };

  /**
   * @brief Persisted pairing data for one Moonlight client.
   */
  struct client_t {
    std::vector<named_cert_t> named_devices;  ///< Persisted Moonlight clients allowed to pair or reconnect.
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t, util::transparent_string_hash, std::equal_to<>> map_id_sess;  ///< Pairing sessions keyed by temporary unique ID.

  /**
   * @brief Get the mutex protecting pairing-session state.
   *
   * @return Mutex serializing pairing-session storage and lifecycle transitions.
   */
  std::mutex &map_id_sess_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  client_t client_root;  ///< In-memory representation of the paired-client database.
  std::atomic<uint32_t> session_id_counter;  ///< Monotonic counter used to allocate GameStream session IDs.

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to GameStream handlers.
   */
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by GameStream handlers.
   */
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  /**
   * @brief Shared HTTP response object passed to redirect and discovery handlers.
   */
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  /**
   * @brief Shared HTTP request object received by redirect and discovery handlers.
   */
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  /**
   * @brief Certificate operations supported by the pairing API.
   */
  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  /**
   * @brief Read a named query argument from the HTTP request map.
   *
   * @param args Parsed query-string argument map.
   * @param name Query parameter name to read.
   * @param default_value Value returned when the parameter is absent.
   * @return Query parameter value, default value, or an empty string.
   */
  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  /**
   * @brief Persist the current state to its backing store.
   */
  void save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  /**
   * @brief Convert a PEM certificate to Sunshine's canonical OpenSSL serialization.
   *
   * @param cert_pem PEM-encoded certificate to canonicalize.
   * @return Canonical PEM text, or an empty string when the certificate is invalid.
   */
  std::string canonical_certificate_pem(const std::string_view cert_pem) {
    auto certificate = crypto::x509(cert_pem);
    return certificate ? crypto::pem(certificate) : std::string {};
  }

  /**
   * @brief Rebuild the GameStream trust stores from enabled paired-client records.
   *
   * @note The caller must hold `client_auth_mutex()`.
   */
  void rebuild_client_cert_chain() {
    cert_chain.clear();
    for (const auto &named_cert : client_root.named_devices) {
      if (!named_cert.enabled) {
        continue;
      }

      auto certificate = crypto::x509(named_cert.cert);
      if (!certificate) {
        BOOST_LOG(warning) << "Ignoring invalid paired-client certificate"sv;
        continue;
      }

      cert_chain.add(std::move(certificate));
    }
  }

  /**
   * @brief Find the unique enabled paired-client record matching an exact certificate identity.
   *
   * @param certificate Parsed client certificate to compare by canonical X.509 identity.
   * @return The unique enabled paired record, or `nullptr` for an unknown, disabled, or ambiguous identity.
   * @note The caller must hold `client_auth_mutex()`.
   */
  const named_cert_t *find_enabled_client(const X509 *certificate) {
    const named_cert_t *matched = nullptr;
    for (const auto &named_cert : client_root.named_devices) {
      if (auto stored_certificate = crypto::x509(named_cert.cert); !stored_certificate || X509_cmp(stored_certificate.get(), certificate) != 0) {
        continue;
      }

      if (matched || !named_cert.enabled) {
        return nullptr;
      }
      matched = &named_cert;
    }
    return matched;
  }

  /**
   * @brief Verify a client certificate against the exact enabled paired identity.
   *
   * @param certificate Parsed client certificate presented during the TLS handshake.
   * @return `nullptr` when authorized, otherwise a non-sensitive error string.
   * @note The caller must hold `client_auth_mutex()`.
   */
  const char *verify_client_certificate(X509 *certificate) {
    if (const auto verification_error = cert_chain.verify(certificate); verification_error) {
      return verification_error;
    }
    return find_enabled_client(certificate) ? nullptr : "Client certificate identity is not enabled";
  }

  /**
   * @brief Resolve the actual request peer to its current paired record.
   * @param server HTTPS server that verified the peer certificate.
   * @param request Request from the verified TLS peer.
   * @return Paired client identity, absent if revoked, disabled, or unverified.
   */
  std::optional<named_cert_t> authenticated_request_client(https_server_t &server, const https_server_t::Request &request) {
    const auto certificate = crypto::x509(server.verified_certificate(request));
    if (!certificate) {
      return std::nullopt;
    }
    std::lock_guard lock {client_auth_mutex()};
    if (const auto *client = find_enabled_client(certificate.get())) {
      return *client;
    }
    return std::nullopt;
  }

  bool is_authorized_stream_client(const std::string_view uuid, const std::string_view cert) {
    const auto certificate = crypto::x509(cert);
    if (!certificate) {
      return false;
    }
    std::lock_guard lock {client_auth_mutex()};
    const auto *client = find_enabled_client(certificate.get());
    return client && client->uuid == uuid;
  }

  /**
   * @brief Verify the paired client identity presented by a TLS peer.
   * @param ssl TLS connection after its handshake.
   * @return The verified certificate in canonical PEM form, or none when unpaired or disabled.
   */
  std::optional<std::string> verify_https_peer(SSL *ssl) {
    crypto::x509_t certificate {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };
    if (!certificate) {
      BOOST_LOG(info) << "unknown -- denied"sv;
      return std::nullopt;
    }

    bool verified = false;
    auto log_result = util::fail_guard([&]() {
      std::array<char, 256> subject_name;
      X509_NAME_oneline(X509_get_subject_name(certificate.get()), subject_name.data(), subject_name.size());
      BOOST_LOG(debug) << subject_name.data() << " -- "sv << (verified ? "verified"sv : "denied"sv);
    });

    std::lock_guard lock {client_auth_mutex()};
    if (const auto error = verify_client_certificate(certificate.get())) {
      BOOST_LOG(warning) << "SSL Verification error :: "sv << error;
      return std::nullopt;
    }
    verified = true;
    return crypto::pem(certificate);
  }

  /**
   * @brief Load state from its backing store.
   */
  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        client.named_devices.emplace_back(named_cert);
      }
    }

    for (auto &named_cert : client.named_devices) {
      auto canonical_certificate = canonical_certificate_pem(named_cert.cert);
      if (!canonical_certificate.empty()) {
        named_cert.cert = std::move(canonical_certificate);
      }
    }

    std::lock_guard lock {client_auth_mutex()};
    client_root = std::move(client);
    rebuild_client_cert_chain();
  }

  /**
   * @brief Add authorized client data.
   *
   * A completed pairing replaces all records with the same exact X.509 identity so legacy duplicate records cannot make the newly paired client fail authorization.
   *
   * @param name Human-readable name to assign.
   * @param cert Certificate data or object used by the operation.
   * @return Persistent UUID for the added client, or an empty string when the certificate is invalid.
   */
  std::string add_authorized_client(const std::string &name, std::string &&cert) {
    auto certificate = crypto::x509(cert);
    if (!certificate) {
      return {};
    }

    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = crypto::pem(certificate);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    const auto uuid = named_cert.uuid;

    std::lock_guard lock {client_auth_mutex()};
    std::erase_if(client_root.named_devices, [&certificate](const named_cert_t &existing_client) {
      auto existing_certificate = crypto::x509(existing_client.cert);
      return existing_certificate && X509_cmp(existing_certificate.get(), certificate.get()) == 0;
    });
    client_root.named_devices.emplace_back(std::move(named_cert));
    rebuild_client_cert_chain();

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
    return uuid;
  }

  /**
   * @brief Create launch session.
   *
   * @param host_audio Host audio.
   * @param args Arguments forwarded to the callable or parser.
   * @param client Paired peer identity verified for this request.
   * @return Constructed launch session object.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args, const named_cert_t &client) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;
    launch_session->manual_video = args.count("sidelantern_manual_video") == 1 && get_arg(args, "sidelantern_manual_video", "0") == "1";

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    auto mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    // Cap the session resolution up front so downstream consumers (e.g. the display device
    // configuration that switches the host display mode) target the actual streamed size
    // instead of the uncapped client request.
    config::cap_stream_resolution(launch_session->width, launch_session->height);
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    launch_session->client_cert = client.cert;
    launch_session->client_name = client.name;
    launch_session->client_uuid = client.uuid;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    std::array<unsigned char, 8> raw_payload;
    RAND_bytes(raw_payload.data(), raw_payload.size());
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  /**
   * @brief Write a completed response to the client waiting for PIN approval.
   *
   * @param sess Pairing session that owns the waiting response.
   * @param tree XML response body to write.
   * @return `true` when a live response was available.
   */
  bool write_pairing_response(pair_session_t &sess, const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &response = sess.async_insert_pin.response;
    if (response.has_left() && response.left()) {
      response.left()->close_connection_after_response = true;
      response.left()->write(data.str());
    } else if (response.has_right() && response.right()) {
      response.right()->close_connection_after_response = true;
      response.right()->write(data.str());
    } else {
      return false;
    }

    response = std::monostate {};
    return true;
  }

  /**
   * @brief Publish the final result of a pairing handshake.
   *
   * @param sess Pairing session whose waiter should be notified.
   * @param success Whether the client completed the authenticated handshake.
   */
  void complete_pairing(pair_session_t &sess, const bool success) {
    {
      std::lock_guard lock {sess.completion->mutex};
      if (sess.completion->result.has_value()) {
        return;
      }
      sess.completion->result = success;
    }
    sess.completion->condition.notify_all();
  }

  /**
   * @brief Expire stale pairing sessions while the session mutex is held.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions_unlocked(const std::chrono::steady_clock::time_point now) {
    for (auto it = map_id_sess.begin(); it != map_id_sess.end();) {
      if (it->second.async_insert_pin.expires_at > now) {
        ++it;
        continue;
      }

      pt::ptree tree;
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      write_pairing_response(it->second, tree);
      complete_pairing(it->second, false);
      it = map_id_sess.erase(it);
    }
  }

  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id) {
    std::scoped_lock lock {map_id_sess_mutex()};
    const auto now = std::chrono::steady_clock::now();
    expire_pair_sessions_unlocked(now);
    pairing_id.clear();

    if (map_id_sess.contains(sess.client.uniqueID)) {
      return pair_session_insert_e::ALREADY_EXISTS;
    }
    if (map_id_sess.size() >= MAX_PENDING_PAIRING_SESSIONS) {
      return pair_session_insert_e::FULL;
    }

    do {
      pairing_id = crypto::rand_alphabet(PAIRING_ID_SIZE, "0123456789abcdef"sv);
    } while (std::ranges::any_of(map_id_sess, [&](const auto &entry) {
      return entry.second.async_insert_pin.id == pairing_id;
    }));

    sess.async_insert_pin.id = pairing_id;
    sess.async_insert_pin.expires_at = now + PAIRING_SESSION_TIMEOUT;
    map_id_sess.try_emplace(sess.client.uniqueID, std::move(sess));
    return pair_session_insert_e::ADDED;
  }

  bool evict_abandoned_pair_session(const std::string &unique_id) {
    std::scoped_lock lock {map_id_sess_mutex()};
    const auto sess_it = map_id_sess.find(unique_id);
    if (sess_it == map_id_sess.end() || sess_it->second.last_phase == PAIR_PHASE::NONE) {
      return false;
    }

    complete_pairing(sess_it->second, false);
    map_id_sess.erase(sess_it);
    return true;
  }

  bool is_valid_pairing_id(const std::string_view pairing_id) {
    return pairing_id.size() == PAIRING_ID_SIZE && std::ranges::all_of(pairing_id, [](const char character) {
             return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
           });
  }

  bool is_valid_pairing_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char character) {
             return character >= '0' && character <= '9';
           });
  }

  bool is_valid_pairing_name(const std::string_view name) {
    return !name.empty() && name.size() <= MAX_PAIRING_CLIENT_NAME_SIZE;
  }

  void expire_pair_sessions(const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(now);
  }

  std::vector<pending_pairing_t> get_pending_pairings() {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    std::vector<const pair_session_t *> pending_sessions;
    pending_sessions.reserve(map_id_sess.size());
    for (const auto &[_, sess] : map_id_sess) {
      if (sess.last_phase == PAIR_PHASE::NONE) {
        pending_sessions.push_back(&sess);
      }
    }
    std::ranges::sort(pending_sessions, {}, [](const pair_session_t *sess) {
      return sess->async_insert_pin.expires_at;
    });

    std::vector<pending_pairing_t> result;
    result.reserve(pending_sessions.size());
    for (const auto *sess : pending_sessions) {
      result.push_back({
        .id = sess->async_insert_pin.id,
        .name = sess->async_insert_pin.device_name,
        .address = sess->async_insert_pin.address,
      });
    }
    return result;
  }

  bool cancel_pairing(const std::string_view pairing_id) {
    if (!is_valid_pairing_id(pairing_id)) {
      return false;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
      return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
    });
    if (sess_it == map_id_sess.end()) {
      return false;
    }

    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", "Pairing request cancelled by operator");
    write_pairing_response(sess_it->second, tree);
    complete_pairing(sess_it->second, false);
    map_id_sess.erase(sess_it);
    return true;
  }

  void remove_session(const pair_session_t &sess) {
    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = map_id_sess.find(sess.client.uniqueID); sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
  }

  /**
   * @brief Return the GameStream pairing failure response.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param status_msg Status msg.
   */
  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string &status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    sess.failed = true;
    complete_pairing(sess, false);
  }

  /**
   * @brief Return the server certificate text for pairing responses.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client-challenge phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the server-challenge response phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client pairing-secret phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    bool paired = false;
    if (same_hash && verify) {
      // The client is now successfully paired and will be authorized to connect
      paired = !add_authorized_client(client.name, std::move(client.cert)).empty();
    }

    tree.put("root.paired", paired ? 1 : 0);
    tree.put("root.<xmlattr>.status_code", 200);
    complete_pairing(sess, paired);
  }

  template<class T>
  struct tunnel;

  /**
   * @brief HTTPS tunnel session used for encrypted client requests.
   */
  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;  ///< To string.
  };

  /**
   * @brief Plain HTTP server wrapper used for non-TLS endpoints.
   */
  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;  ///< To string.
  };

  /**
   * @brief Write req details to the log.
   *
   * @param request HTTP request data from the client.
   */
  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Return a GameStream HTTP not-found response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  /**
   * @brief Start a GameStream pairing session for a `getservercert` request.
   *
   * @tparam T HTTP server transport type.
   * @param response HTTP response retained while Web UI PIN approval is pending.
   * @param request HTTP request containing the client pairing parameters.
   * @param args Parsed request query parameters.
   * @param tree XML property tree used for immediate responses.
   * @param unique_id Client-provided pairing-session identifier.
   * @return `true` when the response remains pending for Web UI PIN approval.
   */
  template<class T>
  bool start_pairing_session(
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> &response,
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> &request,
    const args_t &args,
    pt::ptree &tree,
    const std::string &unique_id
  ) {
    const bool pin_stdin = config::sunshine.flags[config::flag::PIN_STDIN];
    auto make_session = [&]() {
      pair_session_t fresh_sess;
      fresh_sess.client.uniqueID = unique_id;
      fresh_sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
      fresh_sess.async_insert_pin.salt = get_arg(args, "salt");
      fresh_sess.async_insert_pin.device_name = get_arg(args, "devicename");
      fresh_sess.async_insert_pin.address = net::addr_to_normalized_string(request->remote_endpoint().address());
      if (!pin_stdin) {
        fresh_sess.async_insert_pin.response = response;
      }

      BOOST_LOG(debug) << fresh_sess.client.cert;
      return fresh_sess;
    };

    std::string pairing_id;
    auto insert_result = insert_pair_session(make_session(), pairing_id);
    if (insert_result == pair_session_insert_e::ALREADY_EXISTS && evict_abandoned_pair_session(unique_id)) {
      // The previous handshake was abandoned mid-flight; the old session cannot resume,
      // so replace it with a fresh one and retry the insert once.
      insert_result = insert_pair_session(make_session(), pairing_id);
    }
    switch (insert_result) {
      using enum pair_session_insert_e;

      case ALREADY_EXISTS:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put("root.<xmlattr>.status_message", "A pairing session with this uniqueid already exists");
        return false;
      case FULL:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Too many pending pairing sessions");
        return false;
      case ADDED:
        break;
      default:
        // All enumerators are handled above; any other value cannot occur.
        std::unreachable();
    }

    if (!pin_stdin) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_require_pin();
#endif
      return true;
    }

    std::string pin;
    std::cout << "Please insert pin: "sv;
    std::getline(std::cin, pin);

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    const auto sess_it = map_id_sess.find(unique_id);
    if (sess_it == map_id_sess.end() || sess_it->second.async_insert_pin.id != pairing_id) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      return false;
    }

    getservercert(sess_it->second, tree, pin);
    if (sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  /**
   * @brief Dispatch the top-level GameStream pairing request by phase.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    const auto phrase_it = args.find("phrase");
    if (phrase_it != std::end(args) && phrase_it->second == "getservercert"sv) {
      if (start_pairing_session<T>(response, request, args, tree, uniqID)) {
        fg.disable();
      }
      return;
    }
    if (phrase_it != std::end(args) && phrase_it->second == "pairchallenge"sv) {
      tree.put("root.paired", 1);
      tree.put("root.<xmlattr>.status_code", 200);
      return;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    bool pairing_complete = false;
    if (const auto challenge_it = args.find("clientchallenge"); challenge_it != std::end(args)) {
      auto challenge = util::from_hex_vec(challenge_it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (const auto response_it = args.find("serverchallengeresp"); response_it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(response_it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (const auto secret_it = args.find("clientpairingsecret"); secret_it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(secret_it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
      pairing_complete = true;
    } else {
      fail_pair(sess_it->second, tree, "Invalid pairing request");
    }

    if (pairing_complete || sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
  }

  bool pin(const std::string_view pairing_id, const std::string &pin, std::string name) {
    if (!is_valid_pairing_id(pairing_id) || !is_valid_pairing_pin(pin) || !is_valid_pairing_name(name)) {
      return false;
    }

    std::shared_ptr<pairing_completion_t> completion;
    auto completion_deadline = std::chrono::steady_clock::time_point::min();
    {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto now = std::chrono::steady_clock::now();
      expire_pair_sessions_unlocked(now);
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      auto &sess = sess_it->second;
      pt::ptree tree;
      getservercert(sess, tree, pin);
      if (!sess.failed) {
        sess.client.name = std::move(name);
      }

      if (!write_pairing_response(sess, tree) || sess.failed) {
        complete_pairing(sess, false);
        map_id_sess.erase(sess_it);
        return false;
      }

      completion = sess.completion;
      completion_deadline = std::min(sess.async_insert_pin.expires_at, now + config::stream.ping_timeout);
    }

    {
      std::unique_lock lock {completion->mutex};
      if (completion->condition.wait_until(lock, completion_deadline, [&completion]() {
            return completion->result.has_value();
          })) {
        return *completion->result;
      }
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
          return entry.second.async_insert_pin.id == pairing_id && entry.second.completion == completion;
        });
        sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  uint32_t get_codec_mode_flags() {
    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if ((video::active_hevc_mode == 3 || video::active_hevc_mode == 5) && video::last_encoder_probe_supported_dynamic_range_for_codec[1]) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
    }
    if ((video::active_hevc_mode == 4 || video::active_hevc_mode == 5) && video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[1]) {
      codec_mode_flags |= SCM_HEVC_REXT10_444;
    }

    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if ((video::active_av1_mode == 3 || video::active_av1_mode == 5) && video::last_encoder_probe_supported_dynamic_range_for_codec[2]) {
      codec_mode_flags |= SCM_AV1_MAIN10;
    }
    if ((video::active_av1_mode == 4 || video::active_av1_mode == 5) && video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[2]) {
      codec_mode_flags |= SCM_AV1_HIGH10_444;
    }
    return codec_mode_flags;
  }

  /**
   * @brief Build the GameStream server-info response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    const uint32_t codec_mode_flags = get_codec_mode_flags();
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  /**
   * @brief Build the GameStream application list response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);
    proc::refresh(config::stream.file_apps);

    pt::ptree tree;

    auto g = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode >= 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", app));
    }
  }

  namespace {
    using stream_mode_e = display_device::stream_mode_e;  ///< Types of explicit display requests.
    constexpr std::array stream_modes {
      std::pair {"primary"sv, stream_mode_e::primary},
      std::pair {"secondary"sv, stream_mode_e::secondary},
      std::pair {"extend"sv, stream_mode_e::extend}
    };  ///< Stable mode identifiers exposed to clients.

    /** @brief Validated mode and capture target, or a legacy request with no explicit mode. */
    struct stream_request_t {
      std::optional<stream_mode_e> mode;  ///< Explicit mode, if requested.
      display_device::stream_display_t display;  ///< Selected target for an explicit mode.
    };

    /** @brief Resolve a display mode available on this host.
     * @param mode Requested mode.
     * @param devices Display layout snapshot.
     * @param virtual_targets Verified virtual displays eligible for automatic selection.
     * @return Target identity or a stable rejection reason.
     */
    display_device::stream_display_t mode_display([[maybe_unused]] stream_mode_e mode, [[maybe_unused]] const display_device::EnumeratedDeviceList &devices, [[maybe_unused]] const display_device::StringSet &virtual_targets) {
#ifdef _WIN32
      return display_device::select_stream_display(mode, devices, config::video.output_name, virtual_targets);
#else
      return {{}, {}, "unsupported_platform"};
#endif
    }

    /** @brief Populate the GameStream error response for an explicit display request.
     * @param tree Response document.
     * @param status GameStream status code.
     * @param reason Stable failure identifier for SideLantern clients.
     */
    void reject_stream_mode(pt::ptree &tree, int status, std::string_view reason) {
      tree.put("root.<xmlattr>.status_code", status);
      tree.put("root.<xmlattr>.status_message", "The requested SideLantern display mode is unavailable");
      tree.put("root.sidelantern_reason", std::string {reason});
      tree.put("root.gamesession", 0);
      tree.put("root.resume", 0);
    }

    /** @brief Check mode and capture availability before changing any stream or display state.
     * @param args GameStream parameters.
     * @param tree Response document used when rejecting a request.
     * @return Validated request, or no value when it has been rejected.
     */
    std::optional<stream_request_t> validate_stream_request(const args_t &args, pt::ptree &tree) {
      const auto [first, last] = args.equal_range("sidelantern_mode");
      if (first == last) {
        if (display_device::stream_display_claimed()) {
          reject_stream_mode(tree, 409, "capture_busy");
          return std::nullopt;
        }
        return stream_request_t {};
      }
      const auto requested = std::ranges::find(stream_modes, first->second, [](const auto &item) { return item.first; });
      if (requested == stream_modes.end() || std::next(first) != last) {
        reject_stream_mode(tree, 400, "unknown_mode");
        return std::nullopt;
      }
      if (display_device::stream_display_claimed() || rtsp_stream::capture_busy()) {
        reject_stream_mode(tree, 409, "capture_busy");
        return std::nullopt;
      }
      const auto virtual_targets = requested->second == stream_mode_e::primary ? display_device::StringSet {} : display_device::enumerate_mttvdd_targets();
      auto display = mode_display(requested->second, display_device::enumerate_devices(), virtual_targets);
      if (!display.reason.empty()) {
        reject_stream_mode(tree, display.reason == "unsupported_platform" ? 501 : 503, display.reason);
        return std::nullopt;
      }
      return stream_request_t {requested->second, std::move(display)};
    }

    /** @brief Prepare the selected source and attach its lifetime to the RTSP ticket.
     * @param request Validated mode request.
     * @param ticket Protocol state that will be passed to the streaming session.
     * @param tree Response document used when preparation or encoder initialization fails.
     * @return Whether the request can proceed.
     */
    bool prepare_stream_request(const stream_request_t &request, rtsp_stream::launch_session_t &ticket, pt::ptree &tree) {
      if (!request.mode) {
        return true;
      }
      ticket.display_claim = display_device::prepare_stream_display(*request.mode, request.display);
      const auto capture_output = ticket.display_claim ? display_device::map_output_name(request.display.device_id) : std::string {};
      if (capture_output.empty() || video::probe_encoders(capture_output)) {
        reject_stream_mode(tree, 503, "display_preparation_failed");
        return false;
      }
      ticket.required_display_id = request.display.device_id;
      return true;
    }

    /** @brief Return the three display choices and their current availability to a paired client.
     * @param response HTTPS response.
     */
    void stream_mode_list(const resp_https_t &response) {
      const auto devices = display_device::enumerate_devices();
      const auto busy = display_device::stream_display_claimed() || rtsp_stream::capture_busy();
      const auto virtual_targets = busy ? display_device::StringSet {} : display_device::enumerate_mttvdd_targets();
      pt::ptree tree;
      auto &root = tree.add_child("root", pt::ptree {});
      root.put("<xmlattr>.status_code", 200);
      root.put("<xmlattr>.version", 1);
#ifdef _WIN32
      root.put("<xmlattr>.manual_video_version", 1);
#endif
      for (const auto &[id, mode] : stream_modes) {
        const auto target = mode_display(mode, devices, virtual_targets);
        const auto reason = busy ? "capture_busy"sv : target.reason;
        auto &item = root.add_child("mode", pt::ptree {});
        item.put("<xmlattr>.id", std::string {id});
        item.put("<xmlattr>.available", reason.empty() ? 1 : 0);
        item.put("<xmlattr>.reason", std::string {reason});
      }
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    }

    /** @brief Read a single unsigned query value without accepting trailing text or duplicates.
     * @param args HTTP query parameters.
     * @param key Parameter name.
     * @return Parsed value when the field occurs exactly once.
     */
    std::optional<uint32_t> manual_video_query_value(const args_t &args, const char *key) {
      const auto [first, end] = args.equal_range(key);
      if (first == end || std::next(first) != end) {
        return std::nullopt;
      }
      const auto &value = first->second;
      uint32_t result;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      return parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size() ? std::optional {result} : std::nullopt;
    }

    /** @brief Query or queue manual settings for the connected stream of the authenticated device.
     * @param response HTTPS response.
     * @param request HTTPS request and optional settings query.
     * @param client Paired device authenticated by TLS.
     * @param update True for an explicit client update.
     */
    void manual_video(const resp_https_t &response, const req_https_t &request, const named_cert_t &client, bool update) {
      pt::ptree tree;
      auto &root = tree.add_child("root", pt::ptree {});
      auto write = util::fail_guard([&] {
        std::ostringstream data;
        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;
      });
      root.put("<xmlattr>.version", 1);
      const auto session = rtsp_stream::manual_video_session(client.uuid);
      if (!session) {
        root.put("<xmlattr>.status_code", 404);
        root.put("status", "no_manual_video_session");
        return;
      }
      int status = 200;
      if (update) {
        const auto args = request->parse_query_string();
        const auto id = manual_video_query_value(args, "session_id");
        const auto width = manual_video_query_value(args, "width");
        const auto height = manual_video_query_value(args, "height");
        const auto fps = manual_video_query_value(args, "fps");
        const auto bitrate = manual_video_query_value(args, "bitrate_kbps");
        if (!id || !width || !height || !fps || !bitrate || args.size() != 5 ||
            *width < 64 || *width > 7680 || *height < 64 || *height > 7680 || *width % 2 != 0 || *height % 2 != 0 ||
            *width * *height > 7680 * 4320 || *fps < 1 || *fps > 240 || *bitrate < 500 || *bitrate > 250000) {
          status = 400;
          root.put("reason", "invalid_video_settings");
        } else if (*id != session->id) {
          status = 409;
          root.put("reason", "stale_session");
        } else {
          video::video_settings_t settings {static_cast<int>(*width), static_cast<int>(*height), static_cast<int>(*fps), static_cast<int>(*bitrate)};
          if ((config::video.max_bitrate > 0 && settings.bitrate_kbps > config::video.max_bitrate) || config::cap_stream_resolution(settings.width, settings.height)) {
            status = 400;
            root.put("reason", "host_video_limit");
          } else if (!session->settings->request(settings)) {
            status = 409;
            root.put("reason", "video_change_pending");
          } else {
            status = 202;
          }
        }
      }
      const auto snapshot = session->settings->snapshot();
      root.put("<xmlattr>.status_code", status);
      root.put("session_id", session->id);
      root.put("status", std::string {snapshot.status});
      root.put("width", snapshot.current.width);
      root.put("height", snapshot.current.height);
      root.put("fps", snapshot.current.fps);
      root.put("bitrate_kbps", snapshot.current.bitrate_kbps);
    }
  }  // namespace

  /**
   * @brief Launch the requested application for a GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @param client Paired peer identity verified for this request.
   */
  void launch(bool &host_audio, resp_https_t response, req_https_t request, const named_cert_t &client) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    const auto stream_request = validate_stream_request(args, tree);
    if (!stream_request) {
      return;
    }
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args, client);

    if (!stream_request->mode && rtsp_stream::session_count() == 0) {
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (!prepare_stream_request(*stream_request, *launch_session, tree)) {
      return;
    }
    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  /**
   * @brief Resume an existing GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @param client Paired peer identity verified for this request.
   */
  void resume(bool &host_audio, resp_https_t response, req_https_t request, const named_cert_t &client) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    const auto stream_request = validate_stream_request(args, tree);
    if (!stream_request) {
      return;
    }
    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args, client);

    if (no_active_sessions && !stream_request->mode) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (!prepare_stream_request(*stream_request, *launch_session, tree)) {
      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  namespace {
    /** @brief Register the shared authenticated routes for production and HTTPS tests.
     * @param server GameStream HTTPS listener.
     */
    void register_stream_routes(https_server_t &server) {
      auto host_audio = std::make_shared<bool>(false);
      const auto authenticated_stream_handler = [&server, &host_audio](auto handler) {
        return [&server, host_audio, handler](resp_https_t resp, req_https_t req) {
          if (const auto client = authenticated_request_client(server, *req)) {
            handler(*host_audio, std::move(resp), std::move(req), *client);
          } else {
            server.on_verify_failed(std::move(resp), std::move(req));
          }
        };
      };
      server.resource["^/launch$"]["GET"] = authenticated_stream_handler(launch);
      server.resource["^/resume$"]["GET"] = authenticated_stream_handler(resume);
      server.resource["^/sidelantern/v1/stream-modes$"]["GET"] = authenticated_stream_handler(
        [](bool &, const resp_https_t &response, const req_https_t &, const named_cert_t &) { stream_mode_list(response); }
      );
      server.resource["^/sidelantern/v1/manual-video$"]["GET"] = authenticated_stream_handler(
        [](bool &, const resp_https_t &response, const req_https_t &request, const named_cert_t &client) { manual_video(response, request, client, false); }
      );
      server.resource["^/sidelantern/v1/manual-video$"]["POST"] = authenticated_stream_handler(
        [](bool &, const resp_https_t &response, const req_https_t &request, const named_cert_t &client) { manual_video(response, request, client, true); }
      );
    }

#ifdef _WIN32
    /** @brief Access the local file transfer service for the engine process. */
    const std::shared_ptr<file_transfer::service> &local_file_transfers() {
      static const auto transfers = std::make_shared<file_transfer::service>(platf::appdata() / "file-transfer.json");
      return transfers;
    }

    /** @brief Write an authorization failure for the file transfer protocol. */
    void deny_file_transfer(const resp_https_t &response) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized, R"({"error":"device_not_paired"})", {
        {"Content-Type", "application/json"}, {"Cache-Control", "no-store"}
      });
      response->close_connection_after_response = true;
    }

    /** @brief Map paired HTTPS requests to the independent file transfer service. */
    void register_file_routes(https_server_t &server, const std::shared_ptr<file_transfer::service> &transfers) {
      using file_transfer::operation;
      const auto route = [&server, &transfers](const char *path, const char *method, operation action) {
        server.resource[path][method] = [&server, transfers, action](const resp_https_t &resp, const req_https_t &req) {
          const auto client = authenticated_request_client(server, *req);
          if (!client) {
            deny_file_transfer(resp);
            return;
          }
          const auto args = req->parse_query_string();
          const auto type = req->header.find("Content-Type");
          const auto body = action == operation::create_upload || action == operation::append_upload ? req->content.string() : std::string {};
          const auto result = transfers->execute(client->uuid, action, file_transfer::query_t {args.begin(), args.end()},
                                                 type != req->header.end() ? std::string_view {type->second} : std::string_view {}, body);
          resp->write(static_cast<SimpleWeb::StatusCode>(result.status), result.body,
                      SimpleWeb::CaseInsensitiveMultimap {result.headers.begin(), result.headers.end()});
        };
      };
      route("^/sidelantern/v1/files$", "GET", operation::capabilities);
      route("^/sidelantern/v1/files/list$", "GET", operation::list);
      route("^/sidelantern/v1/files/download$", "GET", operation::download);
      route("^/sidelantern/v1/files/uploads$", "POST", operation::create_upload);
      route("^/sidelantern/v1/files/uploads$", "GET", operation::get_upload);
      route("^/sidelantern/v1/files/uploads$", "PUT", operation::append_upload);
      route("^/sidelantern/v1/files/uploads/complete$", "POST", operation::complete_upload);
      route("^/sidelantern/v1/files/uploads$", "DELETE", operation::cancel_upload);
    }
#endif
  }  // namespace

  /**
   * @brief Check whether cel.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  /**
   * @brief Return an application asset requested by the client.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = verify_https_peer;

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
#ifdef _WIN32
      if (req->path == "/sidelantern/v1/files" || req->path.starts_with("/sidelantern/v1/files/")) {
        deny_file_transfer(resp);
        return;
      }
#endif
      pt::ptree tree;
      auto g = util::fail_guard([&tree, &resp]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SunshineHTTPS>(resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    register_stream_routes(https_server);
#ifdef _WIN32
    register_file_routes(https_server, local_file_transfers());
#endif
    https_server.resource["^/cancel$"]["GET"] = cancel;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *web_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(web_server->config.port);
        platf::set_thread_name(name);
        web_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread ssl {accept_and_run, &https_server};
    std::jthread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void erase_all_clients() {
    std::lock_guard lock {client_auth_mutex()};
#ifdef _WIN32
    for (const auto &client : client_root.named_devices) {
      local_file_transfers()->revoke(client.uuid);
    }
#endif
    client_root = {};
    cert_chain.clear();
    save_state();
  }

  bool unpair_client(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex()};
    bool removed = false;
    for (auto it = client_root.named_devices.begin(); it != client_root.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client_root.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    rebuild_client_cert_chain();
    save_state();
#ifdef _WIN32
    if (removed) {
      local_file_transfers()->revoke(uuid);
    }
#endif
    return removed;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    std::lock_guard lock {client_auth_mutex()};
    for (auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        named_cert.enabled = enabled;
        rebuild_client_cert_chain();
        save_state();
#ifdef _WIN32
        if (!enabled) {
          local_file_transfers()->revoke(uuid);
        }
#endif
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Get cert by UUID.
   */
  std::string get_cert_by_uuid(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.cert;
      }
    }
    return {};
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    std::unique_ptr<SimpleWeb::ServerBase<SunshineHTTPS>> make_https_server(
      const std::string &certificate_file,
      const std::string &private_key_file,
      std::shared_ptr<SimpleWeb::ScopeRunner> &handler_scope,
      bool report_verified_client,
      [[maybe_unused]] const std::filesystem::path &file_transfer_settings
    ) {
      /**
       * @brief Expose the production HTTPS handler scope to lifetime regression tests.
       */
      class test_https_server_t: public SunshineHTTPSServer {
      public:
        using SunshineHTTPSServer::handler_runner;
        using SunshineHTTPSServer::SunshineHTTPSServer;
      };

      auto server = std::make_unique<test_https_server_t>(certificate_file, private_key_file);
      handler_scope = server->handler_runner;
      if (report_verified_client) {
        server->verify = verify_https_peer;
        server->on_verify_failed = [](const auto &response, const auto &) {
          response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        };
        register_stream_routes(*server);
#ifdef _WIN32
        if (!file_transfer_settings.empty()) {
          register_file_routes(*server, std::make_shared<file_transfer::service>(file_transfer_settings));
        }
#endif
        server->resource["^/test-verified-client$"]["GET"] = [server = server.get()](const auto &response, const auto &request) {
          if (const auto client = authenticated_request_client(*server, *request)) {
            response->write(client->uuid);
          } else {
            server->on_verify_failed(response, request);
          }
        };
      }
      return server;
    }

    void pair_http(
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response> response,
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request> request
    ) {
      pair<SimpleWeb::HTTP>(std::move(response), std::move(request));
    }

    void reset_client_state() {
      std::lock_guard lock {client_auth_mutex()};
      client_root = {};
      cert_chain.clear();
    }

    std::string add_client(const std::string &name, std::string cert, bool enabled) {
      auto uuid = add_authorized_client(name, std::move(cert));
      if (!uuid.empty() && !enabled) {
        set_client_enabled(uuid, false);
      }
      return uuid;
    }

    bool duplicate_client(const std::string_view uuid) {
      std::lock_guard lock {client_auth_mutex()};
      const auto client_it = std::ranges::find(client_root.named_devices, uuid, &named_cert_t::uuid);
      if (client_it == client_root.named_devices.end()) {
        return false;
      }

      auto duplicate = *client_it;
      duplicate.uuid = uuid_util::uuid_t::generate().string();
      client_root.named_devices.emplace_back(std::move(duplicate));
      rebuild_client_cert_chain();
      save_state();
      return true;
    }

    bool authorize_client_certificate(const std::string_view cert) {
      auto certificate = crypto::x509(cert);
      if (!certificate) {
        return false;
      }

      std::lock_guard lock {client_auth_mutex()};
      return verify_client_certificate(certificate.get()) == nullptr;
    }

    bool complete_pairing(const std::string_view pairing_id, const bool success) {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      nvhttp::complete_pairing(sess_it->second, success);
      map_id_sess.erase(sess_it);
      return true;
    }

    void reload_client_state() {
      load_state();
    }
  }  // namespace test_support
#endif
}  // namespace nvhttp
