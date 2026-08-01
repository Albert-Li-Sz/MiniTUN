#include <CLI/CLI.hpp>

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>

namespace {

struct Options final {
    std::string endpoint;
    std::string server_name{"localhost"};
    std::string ca_certificate_path;
    std::string token_file_path;
    bool expect_auth_failure{false};
    bool expect_goaway{false};
    std::size_t heartbeat_count{1U};
};

[[nodiscard]] minitun::common::Result<minitun::common::SecureString>
read_token(const std::string& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return minitun::common::Result<minitun::common::SecureString>::failure(
            minitun::common::ErrorCode::invalid_argument, "test Token file cannot be opened");
    }
    std::string token{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    if (token.empty()) {
        return minitun::common::Result<minitun::common::SecureString>::failure(
            minitun::common::ErrorCode::invalid_argument, "test Token file is empty");
    }
    minitun::common::SecureString secret{token};
    minitun::common::secure_erase_memory(token.data(), token.capacity());
    return secret;
}

[[nodiscard]] asio::awaitable<minitun::common::Result<void>>
run_protocol(minitun::protocol::TlsStream& stream, const Options& options,
             const minitun::common::SecureString& token) {
    asio::error_code handshake_error;
    co_await stream.async_handshake(asio::ssl::stream_base::client,
                                    asio::redirect_error(asio::use_awaitable, handshake_error));
    if (handshake_error) {
        co_return minitun::common::Result<void>::failure(minitun::common::ErrorCode::tls_error,
                                                         "test TLS handshake failed");
    }

    auto generated_client_id = minitun::common::Id::generate(minitun::common::IdKind::client);
    if (!generated_client_id) {
        co_return minitun::common::Result<void>::failure(generated_client_id.error());
    }
    const std::string client_id = generated_client_id->str();
    minitun::protocol::StateMachine state{minitun::protocol::PeerRole::client,
                                          minitun::protocol::ConnectionKind::control};

    auto hello_payload = minitun::protocol::encode_hello({client_id});
    if (!hello_payload || !state.on_send(minitun::protocol::MessageType::hello)) {
        co_return minitun::common::Result<void>::failure(minitun::common::ErrorCode::internal_error,
                                                         "test HELLO encoding failed");
    }
    const minitun::protocol::Frame hello_frame{minitun::protocol::MessageType::hello, 0U, 1U,
                                               std::move(*hello_payload)};
    auto written = co_await minitun::protocol::async_write_frame(stream, hello_frame);
    if (!written) {
        co_return written;
    }

    auto ack_frame = co_await minitun::protocol::async_read_frame(stream);
    if (!ack_frame || ack_frame->type != minitun::protocol::MessageType::hello_ack ||
        !state.on_receive(ack_frame->type)) {
        co_return minitun::common::Result<void>::failure(minitun::common::ErrorCode::protocol_error,
                                                         "test HELLO_ACK was invalid");
    }
    auto ack = minitun::protocol::decode_hello_ack(ack_frame->payload);
    if (!ack) {
        co_return minitun::common::Result<void>::failure(ack.error());
    }

    const std::int64_t timestamp = minitun::common::unix_seconds_now();
    auto digest = minitun::protocol::compute_authentication_data(token.view(), client_id, timestamp,
                                                                 ack->nonce);
    if (!digest) {
        co_return minitun::common::Result<void>::failure(digest.error());
    }
    auto auth_payload = minitun::protocol::encode_auth({client_id, timestamp, ack->nonce, *digest});
    if (!auth_payload || !state.on_send(minitun::protocol::MessageType::auth)) {
        co_return minitun::common::Result<void>::failure(minitun::common::ErrorCode::internal_error,
                                                         "test AUTH encoding failed");
    }
    const minitun::protocol::Frame auth_frame{minitun::protocol::MessageType::auth, 0U, 2U,
                                              std::move(*auth_payload)};
    written = co_await minitun::protocol::async_write_frame(stream, auth_frame);
    if (!written) {
        co_return written;
    }

    auto auth_result = co_await minitun::protocol::async_read_frame(stream);
    if (!auth_result || !state.on_receive(auth_result->type)) {
        co_return minitun::common::Result<void>::failure(
            minitun::common::ErrorCode::protocol_error, "test authentication response was invalid");
    }
    if (options.expect_auth_failure) {
        if (auth_result->type != minitun::protocol::MessageType::auth_error ||
            !minitun::protocol::decode_auth_error(auth_result->payload)) {
            co_return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::protocol_error,
                "test expected a generic authentication failure");
        }
        co_return minitun::common::Result<void>::success();
    }
    if (auth_result->type != minitun::protocol::MessageType::auth_ok ||
        !minitun::protocol::decode_auth_ok(auth_result->payload)) {
        co_return minitun::common::Result<void>::failure(
            minitun::common::ErrorCode::authentication_failed,
            "test client authentication was rejected");
    }

    std::size_t heartbeats = 0U;
    while (heartbeats < options.heartbeat_count || options.expect_goaway) {
        auto ping_frame = co_await minitun::protocol::async_read_frame(stream);
        if (!ping_frame || !state.on_receive(ping_frame->type)) {
            co_return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::protocol_error, "test heartbeat PING was invalid");
        }
        if (ping_frame->type == minitun::protocol::MessageType::request_workers) {
            if (!minitun::protocol::decode_request_workers(ping_frame->payload)) {
                co_return minitun::common::Result<void>::failure(
                    minitun::common::ErrorCode::protocol_error, "test worker request was invalid");
            }
            continue;
        }
        if (ping_frame->type == minitun::protocol::MessageType::goaway) {
            co_return options.expect_goaway ? minitun::common::Result<void>::success()
                                            : minitun::common::Result<void>::failure(
                                                  minitun::common::ErrorCode::connection_failed,
                                                  "test server sent an unexpected GOAWAY");
        }
        if (ping_frame->type != minitun::protocol::MessageType::ping) {
            co_return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::protocol_error, "test heartbeat PING was invalid");
        }
        auto ping = minitun::protocol::decode_heartbeat(ping_frame->payload);
        if (!ping) {
            co_return minitun::common::Result<void>::failure(ping.error());
        }
        auto pong_payload = minitun::protocol::encode_heartbeat(*ping);
        if (!pong_payload || !state.on_send(minitun::protocol::MessageType::pong)) {
            co_return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::internal_error, "test PONG encoding failed");
        }
        const minitun::protocol::Frame pong_frame{minitun::protocol::MessageType::pong, 0U,
                                                  ping_frame->request_id, std::move(*pong_payload)};
        written = co_await minitun::protocol::async_write_frame(stream, pong_frame);
        if (!written) {
            co_return written;
        }
        ++heartbeats;
    }
    co_return minitun::common::Result<void>::success();
}

int run(const Options& options) {
    auto endpoint = minitun::common::Endpoint::parse(options.endpoint);
    auto token = read_token(options.token_file_path);
    auto context = minitun::protocol::make_client_tls_context(
        {.ca_certificate_path = options.ca_certificate_path});
    if (!endpoint || !token || !context) {
        return EXIT_FAILURE;
    }

    asio::io_context io_context;
    minitun::protocol::TlsStream stream{io_context, **context};
    auto configured =
        minitun::protocol::configure_client_tls_stream(stream, options.server_name, false);
    if (!configured) {
        return EXIT_FAILURE;
    }

    asio::ip::tcp::resolver resolver{io_context};
    asio::error_code connect_error;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto endpoints =
            resolver.resolve(endpoint->host(), std::to_string(endpoint->port()), connect_error);
        if (!connect_error) {
            asio::connect(stream.lowest_layer(), endpoints, connect_error);
        }
        if (!connect_error) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    if (connect_error) {
        return EXIT_FAILURE;
    }

    std::optional<minitun::common::Result<void>> outcome;
    asio::co_spawn(
        io_context, run_protocol(stream, options, *token),
        [&outcome](const std::exception_ptr failure, minitun::common::Result<void> result) {
            if (failure) {
                outcome = minitun::common::Result<void>::failure(
                    minitun::common::ErrorCode::internal_error, "test TLS client coroutine failed");
            } else {
                outcome = std::move(result);
            }
        });
    io_context.run();
    minitun::protocol::close_tls_stream(stream);
    return outcome.has_value() && *outcome ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"MiniTun TLS authentication integration client"};
    Options options;
    app.add_option("--endpoint", options.endpoint)->required();
    app.add_option("--server-name", options.server_name)->capture_default_str();
    app.add_option("--ca-cert", options.ca_certificate_path)->required();
    app.add_option("--token-file", options.token_file_path)->required();
    app.add_flag("--expect-auth-failure", options.expect_auth_failure);
    app.add_flag("--expect-goaway", options.expect_goaway);
    app.add_option("--heartbeat-count", options.heartbeat_count)
        ->check(CLI::Range(0U, 10U))
        ->capture_default_str();
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }
    return run(options);
}
