#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/frame.hpp>
#include <minitun/ipc/local_client.hpp>
#include <minitun/ipc/local_server.hpp>
#include <minitun/ipc/protocol.hpp>

#include "../../src/ipc/local_internal.hpp"

namespace minitun::ipc {
namespace {

using namespace std::chrono_literals;

class TemporaryIpcDirectory final {
  public:
    TemporaryIpcDirectory() {
        const auto temporary_root =
            std::filesystem::canonical(std::filesystem::temp_directory_path());
        std::string pattern = (temporary_root / "minitun-ipc-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* const created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string{"mkdtemp failed: "} + std::strerror(errno));
        }
        directory_ = created;
    }

    ~TemporaryIpcDirectory() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TemporaryIpcDirectory(const TemporaryIpcDirectory&) = delete;
    TemporaryIpcDirectory& operator=(const TemporaryIpcDirectory&) = delete;

    [[nodiscard]] std::string socket_path(std::string_view name = "minitun.sock") const {
        return (directory_ / name).string();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory_; }

  private:
    std::filesystem::path directory_;
};

[[nodiscard]] Request make_request(Json params = Json::object(), std::string method = "test.echo") {
    auto id = common::Id::generate(common::IdKind::request);
    if (!id) {
        throw std::runtime_error{"failed to generate request ID for IPC transport test"};
    }
    return Request{kProtocolVersion, std::move(id).value(), std::move(method), std::move(params)};
}

class RunningServer final {
  public:
    explicit RunningServer(LocalServerOptions options)
        : dispatcher_(std::make_shared<Dispatcher>()),
          server_(io_context_, dispatcher_, std::move(options)) {}

    ~RunningServer() noexcept { stop(); }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    [[nodiscard]] common::Result<void> register_handler(std::string method, MethodHandler handler) {
        return dispatcher_->register_handler(std::move(method), std::move(handler));
    }

    [[nodiscard]] common::Result<void> start(std::size_t thread_count = 2U) {
        auto started = server_.start();
        if (!started) {
            return started;
        }
        running_ = true;
        try {
            threads_.reserve(thread_count);
            for (std::size_t index = 0; index < thread_count; ++index) {
                threads_.emplace_back([this] {
                    try {
                        io_context_.run();
                    } catch (...) {
                        thread_failed_.store(true, std::memory_order_relaxed);
                    }
                });
            }
        } catch (...) {
            server_.stop();
            join_threads();
            running_ = false;
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "failed to start IPC test threads");
        }
        return common::Result<void>::success();
    }

    void stop() noexcept {
        if (running_) {
            server_.stop();
            running_ = false;
        }
        join_threads();
    }

    [[nodiscard]] LocalServer& server() noexcept { return server_; }
    [[nodiscard]] bool thread_failed() const noexcept {
        return thread_failed_.load(std::memory_order_relaxed);
    }

  private:
    void join_threads() noexcept {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }

    asio::io_context io_context_;
    std::shared_ptr<Dispatcher> dispatcher_;
    LocalServer server_;
    std::vector<std::thread> threads_;
    std::atomic_bool thread_failed_{false};
    bool running_{false};
};

void register_echo_handler(RunningServer& server) {
    const auto registered = server.register_handler(
        "test.echo", [](const Request& request) -> common::Result<Json> { return request.params; });
    if (!registered) {
        throw std::runtime_error{"failed to register IPC test echo handler"};
    }
}

[[nodiscard]] bool wait_for_connection_count(LocalServer& server, std::size_t expected) {
    for (std::size_t attempt = 0; attempt < 500U; ++attempt) {
        if (server.active_connections() == expected) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

[[nodiscard]] std::string read_one_frame(asio::local::stream_protocol::socket& socket) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    asio::read(socket, asio::buffer(header));
    const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
                               (static_cast<std::uint32_t>(header[1]) << 16U) |
                               (static_cast<std::uint32_t>(header[2]) << 8U) |
                               static_cast<std::uint32_t>(header[3]);
    if (size == 0U || size > kDefaultMaxFrameSize) {
        throw std::runtime_error{"test peer returned an invalid IPC frame length"};
    }
    std::string payload(size, '\0');
    asio::read(socket, asio::buffer(payload));
    return payload;
}

TEST(IpcTransportTest, RoundTripsAndCreatesExactPrivateSocketPermissions) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    struct stat status{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &status), 0);
    EXPECT_TRUE(S_ISSOCK(status.st_mode));
    EXPECT_EQ(static_cast<std::uint32_t>(status.st_mode) & 0777U, kDefaultSocketMode);
    EXPECT_EQ(status.st_uid, ::geteuid());
    EXPECT_EQ(status.st_gid, ::getegid());

    struct stat lock_status{};
    ASSERT_EQ(::lstat((socket_path + ".lock").c_str(), &lock_status), 0);
    EXPECT_TRUE(S_ISREG(lock_status.st_mode));
    EXPECT_EQ(static_cast<std::uint32_t>(lock_status.st_mode) & 0777U, 0600U);
    EXPECT_EQ(lock_status.st_uid, ::geteuid());
    EXPECT_EQ(lock_status.st_nlink, 1);

    LocalClient client{LocalClientOptions{.socket_path = socket_path}};
    const auto request = make_request(Json{{"sequence", 7}, {"label", "hello"}});
    const auto response = client.request(request);

    ASSERT_TRUE(response) << response.error();
    ASSERT_TRUE(response->ok());
    ASSERT_NE(response->result(), nullptr);
    EXPECT_EQ(*response->result(), request.params);
    EXPECT_EQ(response->request_id(), request.request_id);

    server.stop();
    EXPECT_FALSE(std::filesystem::exists(socket_path));
    EXPECT_FALSE(server.thread_failed());
}

TEST(IpcTransportTest, ServesConcurrentClientsWithoutMixingResponses) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(server);
    const auto started = server.start(4U);
    ASSERT_TRUE(started) << started.error();

    const LocalClient client{LocalClientOptions{.socket_path = socket_path}};
    std::vector<std::future<bool>> calls;
    calls.reserve(24U);
    for (std::size_t index = 0; index < 24U; ++index) {
        calls.emplace_back(std::async(std::launch::async, [&client, index] {
            const auto request = make_request(Json{{"sequence", index}});
            const auto response = client.request(request);
            return response && response->ok() && response->request_id() == request.request_id &&
                   response->result() != nullptr && *response->result() == request.params;
        }));
    }
    for (auto& call : calls) {
        EXPECT_TRUE(call.get());
    }

    server.stop();
    EXPECT_FALSE(server.thread_failed());
}

TEST(IpcTransportTest, MalformedClientsDoNotAffectLaterRequests) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    {
        asio::io_context io_context;
        asio::local::stream_protocol::socket socket{io_context};
        socket.connect(asio::local::stream_protocol::endpoint{socket_path});
        constexpr std::array<std::uint8_t, kFrameHeaderSize> oversized{
            0x00U,
            0x10U,
            0x00U,
            0x01U,
        };
        asio::write(socket, asio::buffer(oversized));
    }
    {
        asio::io_context io_context;
        asio::local::stream_protocol::socket socket{io_context};
        socket.connect(asio::local::stream_protocol::endpoint{socket_path});
        const auto invalid_json = encode_frame("not-json");
        ASSERT_TRUE(invalid_json) << invalid_json.error();
        asio::write(socket, asio::buffer(*invalid_json));
    }

    LocalClient client{LocalClientOptions{.socket_path = socket_path}};
    const auto response = client.request(make_request(Json{{"healthy", true}}));
    ASSERT_TRUE(response) << response.error();
    EXPECT_TRUE(response->ok());
    server.stop();
    EXPECT_FALSE(server.thread_failed());
}

TEST(IpcTransportTest, AcceptsAValidRequestSplitAcrossHeaderAndBodyWrites) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    const auto request = make_request(Json{{"fragmented", true}});
    const auto payload = serialize_request(request);
    ASSERT_TRUE(payload) << payload.error();
    const auto frame = encode_frame(*payload);
    ASSERT_TRUE(frame) << frame.error();

    asio::io_context io_context;
    asio::local::stream_protocol::socket socket{io_context};
    socket.connect(asio::local::stream_protocol::endpoint{socket_path});
    for (const std::uint8_t byte : *frame) {
        asio::write(socket, asio::buffer(&byte, 1U));
    }

    const auto response = parse_response(read_one_frame(socket));
    ASSERT_TRUE(response) << response.error();
    ASSERT_TRUE(response->ok());
    EXPECT_EQ(response->request_id(), request.request_id);
    ASSERT_NE(response->result(), nullptr);
    EXPECT_EQ(*response->result(), request.params);
    server.stop();
}

TEST(IpcTransportTest, ServesAtMostOneRequestPerConnection) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    const auto first_payload = serialize_request(make_request(Json{{"sequence", 1}}));
    const auto second_payload = serialize_request(make_request(Json{{"sequence", 2}}));
    ASSERT_TRUE(first_payload) << first_payload.error();
    ASSERT_TRUE(second_payload) << second_payload.error();
    const auto first_frame = encode_frame(*first_payload);
    const auto second_frame = encode_frame(*second_payload);
    ASSERT_TRUE(first_frame) << first_frame.error();
    ASSERT_TRUE(second_frame) << second_frame.error();

    std::vector<std::uint8_t> requests = *first_frame;
    requests.insert(requests.end(), second_frame->begin(), second_frame->end());
    asio::io_context io_context;
    asio::local::stream_protocol::socket socket{io_context};
    socket.connect(asio::local::stream_protocol::endpoint{socket_path});
    asio::write(socket, asio::buffer(requests));
    socket.shutdown(asio::local::stream_protocol::socket::shutdown_send);

    FrameDecoder decoder;
    std::size_t response_count = 0;
    std::array<std::uint8_t, 1024> read_buffer{};
    for (;;) {
        asio::error_code error;
        const std::size_t bytes_read = socket.read_some(asio::buffer(read_buffer), error);
        if (error == asio::error::eof) {
            break;
        }
        ASSERT_FALSE(error) << error.message();
        const auto decoded =
            decoder.feed(std::span<const std::uint8_t>{read_buffer.data(), bytes_read});
        ASSERT_TRUE(decoded) << decoded.error();
        response_count += decoded->size();
    }
    EXPECT_LE(response_count, 1U);
    EXPECT_TRUE(decoder.finish());

    const auto healthy = LocalClient{LocalClientOptions{.socket_path = socket_path}}.request(
        make_request(Json{{"healthy", true}}));
    ASSERT_TRUE(healthy) << healthy.error();
    EXPECT_TRUE(healthy->ok());
    server.stop();
}

TEST(IpcTransportTest, ServerDeadlineClosesAStalledPartialRequestOnly) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{
        .socket_path = socket_path,
        .request_timeout = 200ms,
    }};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    asio::io_context raw_io_context;
    asio::local::stream_protocol::socket stalled{raw_io_context};
    stalled.connect(asio::local::stream_protocol::endpoint{socket_path});
    ASSERT_TRUE(wait_for_connection_count(server.server(), 1U));
    constexpr std::array<std::uint8_t, 5U> partial{
        0x00U, 0x00U, 0x00U, 0x64U, static_cast<std::uint8_t>('{'),
    };
    asio::write(stalled, asio::buffer(partial));
    ASSERT_TRUE(wait_for_connection_count(server.server(), 0U));

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 1s,
        .request_timeout = 1s,
    }};
    const auto healthy = client.request(make_request(Json{{"healthy", true}}));
    ASSERT_TRUE(healthy) << healthy.error();
    EXPECT_TRUE(healthy->ok());
    server.stop();
}

TEST(IpcTransportTest, ClientRequestDeadlineCancelsAStalledHandlerResponse) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{.socket_path = socket_path}};
    std::promise<void> release_handler;
    const std::shared_future<void> handler_gate = release_handler.get_future().share();
    ASSERT_TRUE(server.register_handler("test.slow",
                                        [handler_gate](const Request&) -> common::Result<Json> {
                                            static_cast<void>(handler_gate.wait_for(2s));
                                            return Json::object();
                                        }));
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 500ms,
        .request_timeout = 25ms,
    }};
    const auto response = client.request(make_request(Json::object(), "test.slow"));
    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code(), common::ErrorCode::connection_timeout);

    release_handler.set_value();
    server.stop();
    EXPECT_FALSE(server.thread_failed());
}

TEST(IpcTransportTest, MapsMalformedPeerResponsesToProtocolErrors) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    const auto malformed_response = encode_frame("{\"version\":1}");
    ASSERT_TRUE(malformed_response) << malformed_response.error();

    asio::io_context peer_io_context;
    asio::local::stream_protocol::acceptor acceptor{
        peer_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };
    std::atomic_bool peer_failed{false};
    std::thread peer{[&] {
        try {
            asio::local::stream_protocol::socket socket{peer_io_context};
            acceptor.accept(socket);
            static_cast<void>(read_one_frame(socket));
            asio::write(socket, asio::buffer(*malformed_response));
        } catch (...) {
            peer_failed.store(true, std::memory_order_relaxed);
        }
    }};

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 1s,
        .request_timeout = 1s,
    }};
    const auto response = client.request(make_request());
    peer.join();

    EXPECT_FALSE(peer_failed.load(std::memory_order_relaxed));
    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code(), common::ErrorCode::protocol_error);
    EXPECT_EQ(response.error().message(), "IPC peer returned a malformed response");
}

TEST(IpcTransportTest, ClientRejectsResponseWithMismatchedRequestId) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();

    asio::io_context peer_io_context;
    asio::local::stream_protocol::acceptor acceptor{
        peer_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };
    std::atomic_bool peer_failed{false};
    std::thread peer{[&] {
        try {
            asio::local::stream_protocol::socket socket{peer_io_context};
            acceptor.accept(socket);
            static_cast<void>(read_one_frame(socket));
            auto other_id = common::Id::generate(common::IdKind::request);
            if (!other_id) {
                throw std::runtime_error("failed to generate mismatched response ID");
            }
            auto serialized = serialize_response(
                Response::success(std::move(other_id).value(), Json{{"ok", true}}));
            if (!serialized) {
                throw std::runtime_error("failed to serialize mismatched response");
            }
            auto framed = encode_frame(*serialized);
            if (!framed) {
                throw std::runtime_error("failed to frame mismatched response");
            }
            asio::write(socket, asio::buffer(*framed));
        } catch (...) {
            peer_failed.store(true, std::memory_order_relaxed);
        }
    }};

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 1s,
        .request_timeout = 1s,
    }};
    const auto response = client.request(make_request());
    peer.join();

    EXPECT_FALSE(peer_failed.load(std::memory_order_relaxed));
    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code(), common::ErrorCode::protocol_error);
    EXPECT_EQ(response.error().message(), "IPC response request_id does not match the request");
}

TEST(IpcTransportTest, ClientRejectsExtraResponsesOnOneConnection) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();

    asio::io_context peer_io_context;
    asio::local::stream_protocol::acceptor acceptor{
        peer_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };
    std::atomic_bool peer_failed{false};
    std::thread peer{[&] {
        try {
            asio::local::stream_protocol::socket socket{peer_io_context};
            acceptor.accept(socket);
            auto request = parse_request(read_one_frame(socket));
            if (!request) {
                throw std::runtime_error("failed to parse peer request");
            }
            auto serialized = serialize_response(
                Response::success(request->request_id, Json{{"ok", true}}));
            if (!serialized) {
                throw std::runtime_error("failed to serialize peer response");
            }
            auto framed = encode_frame(*serialized);
            if (!framed) {
                throw std::runtime_error("failed to frame peer response");
            }
            asio::write(socket, asio::buffer(*framed));
            asio::write(socket, asio::buffer(*framed));
        } catch (...) {
            peer_failed.store(true, std::memory_order_relaxed);
        }
    }};

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 1s,
        .request_timeout = 1s,
    }};
    const auto response = client.request(make_request());
    peer.join();

    EXPECT_FALSE(peer_failed.load(std::memory_order_relaxed));
    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code(), common::ErrorCode::protocol_error);
    EXPECT_EQ(response.error().message(), "IPC peer sent an unexpected extra response");
}

TEST(IpcTransportTest, ClientMapsTruncatedAndEmptyPeerClosures) {
    {
        TemporaryIpcDirectory temporary;
        const std::string socket_path = temporary.socket_path("truncated.sock");
        asio::io_context peer_io_context;
        asio::local::stream_protocol::acceptor acceptor{
            peer_io_context,
            asio::local::stream_protocol::endpoint{socket_path},
        };
        std::atomic_bool peer_failed{false};
        std::thread peer{[&] {
            try {
                asio::local::stream_protocol::socket socket{peer_io_context};
                acceptor.accept(socket);
                static_cast<void>(read_one_frame(socket));
                const auto framed = encode_frame("{\"version\":1}");
                if (!framed) {
                    throw std::runtime_error("failed to frame truncated response");
                }
                asio::write(socket, asio::buffer(*framed, framed->size() / 2U));
            } catch (...) {
                peer_failed.store(true, std::memory_order_relaxed);
            }
        }};

        LocalClient client{LocalClientOptions{
            .socket_path = socket_path,
            .connect_timeout = 1s,
            .request_timeout = 1s,
        }};
        const auto response = client.request(make_request());
        peer.join();

        EXPECT_FALSE(peer_failed.load(std::memory_order_relaxed));
        ASSERT_FALSE(response);
        EXPECT_EQ(response.error().code(), common::ErrorCode::protocol_error);
        EXPECT_EQ(response.error().message(), "IPC stream ended in the middle of a frame");
    }
    {
        TemporaryIpcDirectory temporary;
        const std::string socket_path = temporary.socket_path("empty.sock");
        asio::io_context peer_io_context;
        asio::local::stream_protocol::acceptor acceptor{
            peer_io_context,
            asio::local::stream_protocol::endpoint{socket_path},
        };
        std::atomic_bool peer_failed{false};
        std::thread peer{[&] {
            try {
                asio::local::stream_protocol::socket socket{peer_io_context};
                acceptor.accept(socket);
                static_cast<void>(read_one_frame(socket));
            } catch (...) {
                peer_failed.store(true, std::memory_order_relaxed);
            }
        }};

        LocalClient client{LocalClientOptions{
            .socket_path = socket_path,
            .connect_timeout = 1s,
            .request_timeout = 1s,
        }};
        const auto response = client.request(make_request());
        peer.join();

        EXPECT_FALSE(peer_failed.load(std::memory_order_relaxed));
        ASSERT_FALSE(response);
        EXPECT_EQ(response.error().code(), common::ErrorCode::ipc_error);
        EXPECT_EQ(response.error().message(), "IPC peer closed before responding");
    }
}

TEST(IpcTransportTest, ClientRejectsOversizedAndInvalidOutboundRequests) {
    const auto request = make_request(Json{{"payload", std::string(512U, 'x')}});
    const auto oversized =
        LocalClient{LocalClientOptions{
            .socket_path = "/tmp/minitun-unused.sock",
            .max_message_size = 128U,
        }}.request(request);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code(), common::ErrorCode::frame_too_large);

    const auto zero_request_timeout = LocalClient{
        LocalClientOptions{
            .socket_path = "/tmp/minitun-unused.sock",
            .request_timeout = 0ms,
        }}.request(request);
    ASSERT_FALSE(zero_request_timeout);
    EXPECT_EQ(zero_request_timeout.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcTransportTest, ServerDeadlineContainsABlockedHandlerWithoutFreezingIo) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{
        .socket_path = socket_path,
        .request_timeout = 500ms,
    }};
    std::promise<void> release_handler;
    const std::shared_future<void> handler_gate = release_handler.get_future().share();
    ASSERT_TRUE(server.register_handler("test.blocked",
                                        [handler_gate](const Request&) -> common::Result<Json> {
                                            static_cast<void>(handler_gate.wait_for(2s));
                                            return Json::object();
                                        }));
    register_echo_handler(server);
    const auto started = server.start(1U);
    ASSERT_TRUE(started) << started.error();

    const LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 1s,
        .request_timeout = 2s,
    }};
    auto blocked_call = std::async(std::launch::async, [&client] {
        return client.request(make_request(Json::object(), "test.blocked"));
    });
    ASSERT_TRUE(wait_for_connection_count(server.server(), 1U));
    ASSERT_TRUE(wait_for_connection_count(server.server(), 0U));
    const auto blocked_result = blocked_call.get();
    EXPECT_FALSE(blocked_result);

    const auto healthy = client.request(make_request(Json{{"healthy", true}}));
    ASSERT_TRUE(healthy) << healthy.error();
    EXPECT_TRUE(healthy->ok());

    release_handler.set_value();
    server.stop();
    EXPECT_FALSE(server.thread_failed());
}

TEST(IpcTransportTest, EnforcesConnectionLimitAndRecoversCapacity) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer server{LocalServerOptions{
        .socket_path = socket_path,
        .max_connections = 1U,
    }};
    register_echo_handler(server);
    const auto started = server.start();
    ASSERT_TRUE(started) << started.error();

    asio::io_context raw_io_context;
    asio::local::stream_protocol::socket held_socket{raw_io_context};
    held_socket.connect(asio::local::stream_protocol::endpoint{socket_path});
    ASSERT_TRUE(wait_for_connection_count(server.server(), 1U));

    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .connect_timeout = 500ms,
        .request_timeout = 500ms,
    }};
    const auto rejected = client.request(make_request());
    EXPECT_FALSE(rejected);

    held_socket.close();
    ASSERT_TRUE(wait_for_connection_count(server.server(), 0U));
    const auto recovered = client.request(make_request(Json{{"recovered", true}}));
    ASSERT_TRUE(recovered) << recovered.error();
    EXPECT_TRUE(recovered->ok());
    server.stop();
}

TEST(IpcTransportTest, RefusesRegularFilesAndSymbolicLinksWithoutChangingThem) {
    TemporaryIpcDirectory temporary;
    const std::string regular_path = temporary.socket_path("regular");
    {
        std::ofstream output{regular_path};
        ASSERT_TRUE(output);
        output << "preserve-me";
    }

    asio::io_context regular_io_context;
    auto regular_dispatcher = std::make_shared<Dispatcher>();
    LocalServer regular_server{
        regular_io_context,
        regular_dispatcher,
        LocalServerOptions{.socket_path = regular_path},
    };
    const auto regular_start = regular_server.start();
    ASSERT_FALSE(regular_start);
    EXPECT_EQ(regular_start.error().code(), common::ErrorCode::already_exists);
    std::ifstream input{regular_path};
    std::string contents;
    input >> contents;
    EXPECT_EQ(contents, "preserve-me");

    const std::string target_path = temporary.socket_path("target");
    const std::string link_path = temporary.socket_path("link");
    {
        std::ofstream output{target_path};
        ASSERT_TRUE(output);
        output << "target-data";
    }
    std::filesystem::create_symlink(target_path, link_path);

    asio::io_context link_io_context;
    auto link_dispatcher = std::make_shared<Dispatcher>();
    LocalServer link_server{
        link_io_context,
        link_dispatcher,
        LocalServerOptions{.socket_path = link_path},
    };
    const auto link_start = link_server.start();
    ASSERT_FALSE(link_start);
    EXPECT_EQ(link_start.error().code(), common::ErrorCode::already_exists);
    EXPECT_TRUE(std::filesystem::is_symlink(link_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(target_path));
}

TEST(IpcTransportTest, RefusesSymbolicLinksInSocketDirectoryAncestors) {
    TemporaryIpcDirectory temporary;
    const auto real_directory = temporary.path() / "real";
    const auto linked_directory = temporary.path() / "linked";
    ASSERT_TRUE(std::filesystem::create_directory(real_directory));
    std::filesystem::create_directory_symlink(real_directory, linked_directory);
    const std::string socket_path = (linked_directory / "minitun.sock").string();

    asio::io_context io_context;
    auto dispatcher = std::make_shared<Dispatcher>();
    LocalServer server{
        io_context,
        dispatcher,
        LocalServerOptions{.socket_path = socket_path},
    };
    const auto started = server.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code(), common::ErrorCode::permission_denied);
    EXPECT_FALSE(std::filesystem::exists(real_directory / "minitun.sock"));
}

TEST(IpcTransportTest, DestructionWaitsForAcceptedDispatcherWork) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    asio::io_context io_context;
    auto dispatcher = std::make_shared<Dispatcher>();
    std::promise<void> handler_entered;
    std::promise<void> release_handler;
    const std::shared_future<void> release_signal = release_handler.get_future().share();
    const auto registered = dispatcher->register_handler(
        "test.block", [&handler_entered, release_signal](const Request&) -> common::Result<Json> {
            handler_entered.set_value();
            release_signal.wait();
            return Json::object();
        });
    ASSERT_TRUE(registered) << registered.error();

    auto server = std::make_unique<LocalServer>(io_context, dispatcher,
                                                LocalServerOptions{.socket_path = socket_path});
    const auto started = server->start();
    ASSERT_TRUE(started) << started.error();
    std::thread io_worker{[&io_context] { io_context.run(); }};
    LocalClient client{LocalClientOptions{
        .socket_path = socket_path,
        .request_timeout = 2s,
    }};
    auto client_call = std::async(std::launch::async, [&client] {
        return client.request(make_request(Json::object(), "test.block"));
    });

    auto entered = handler_entered.get_future();
    if (entered.wait_for(2s) != std::future_status::ready) {
        release_handler.set_value();
        server->stop();
        io_context.stop();
        static_cast<void>(client_call.wait_for(2s));
        io_worker.join();
        FAIL() << "dispatcher handler did not start";
        return;
    }

    auto destruction = std::async(std::launch::async, [&server] { server.reset(); });
    EXPECT_EQ(destruction.wait_for(50ms), std::future_status::timeout);
    release_handler.set_value();
    ASSERT_EQ(destruction.wait_for(2s), std::future_status::ready);
    destruction.get();
    io_context.stop();
    ASSERT_EQ(client_call.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(client_call.get());
    io_worker.join();
}

TEST(IpcTransportTest, RefusesAnActiveSocketButReplacesAStaleOwnedSocket) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer active{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(active);
    const auto active_started = active.start();
    ASSERT_TRUE(active_started) << active_started.error();

    asio::io_context second_io_context;
    auto second_dispatcher = std::make_shared<Dispatcher>();
    LocalServer second{
        second_io_context,
        second_dispatcher,
        LocalServerOptions{.socket_path = socket_path},
    };
    const auto collision = second.start();
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code(), common::ErrorCode::already_exists);

    LocalClient client{LocalClientOptions{.socket_path = socket_path}};
    const auto still_healthy = client.request(make_request());
    ASSERT_TRUE(still_healthy) << still_healthy.error();
    active.stop();

    asio::io_context stale_io_context;
    asio::local::stream_protocol::acceptor stale_acceptor{
        stale_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };
    stale_acceptor.close();
    ASSERT_TRUE(std::filesystem::exists(socket_path));

    RunningServer recovered{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(recovered);
    const auto recovered_start = recovered.start();
    ASSERT_TRUE(recovered_start) << recovered_start.error();
    const auto response =
        LocalClient{LocalClientOptions{.socket_path = socket_path}}.request(make_request());
    ASSERT_TRUE(response) << response.error();
    recovered.stop();
}

TEST(IpcTransportTest, RefusesToRemoveAStaleSocketWithAnUnexpectedOwnerContract) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    asio::io_context stale_io_context;
    asio::local::stream_protocol::acceptor stale_acceptor{
        stale_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };
    stale_acceptor.close();
    struct stat before{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &before), 0);

    const auto current_uid = static_cast<std::uint32_t>(::geteuid());
    const std::uint32_t unexpected_uid = current_uid == std::numeric_limits<std::uint32_t>::max()
                                             ? current_uid - 1U
                                             : current_uid + 1U;
    asio::io_context io_context;
    auto dispatcher = std::make_shared<Dispatcher>();
    LocalServer server{
        io_context,
        dispatcher,
        LocalServerOptions{
            .socket_path = socket_path,
            .owner_uid = unexpected_uid,
        },
    };
    const auto started = server.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code(), common::ErrorCode::permission_denied);

    struct stat after{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &after), 0);
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);
}

TEST(IpcTransportTest, RefusesAnUntrustedWritableParentDirectory) {
    const std::string socket_path = "/tmp/minitun-ipc-untrusted-" +
                                    std::to_string(static_cast<long long>(::getpid())) + ".sock";
    asio::io_context io_context;
    auto dispatcher = std::make_shared<Dispatcher>();
    LocalServer server{
        io_context,
        dispatcher,
        LocalServerOptions{.socket_path = socket_path},
    };
    const auto started = server.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code(), common::ErrorCode::permission_denied);
    EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcTransportTest, StopDoesNotRemoveAReplacementSocketInode) {
    TemporaryIpcDirectory temporary;
    const std::string socket_path = temporary.socket_path();
    RunningServer original{LocalServerOptions{.socket_path = socket_path}};
    register_echo_handler(original);
    const auto started = original.start();
    ASSERT_TRUE(started) << started.error();

    ASSERT_EQ(::unlink(socket_path.c_str()), 0);
    asio::io_context replacement_io_context;
    asio::local::stream_protocol::acceptor replacement{
        replacement_io_context,
        asio::local::stream_protocol::endpoint{socket_path},
    };

    original.stop();
    struct stat status{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &status), 0);
    EXPECT_TRUE(S_ISSOCK(status.st_mode));

    replacement.close();
    EXPECT_TRUE(std::filesystem::remove(socket_path));
}

TEST(IpcTransportTest, RejectsEveryInvalidServerOptionBeforeCreatingSocketState) {
    TemporaryIpcDirectory temporary;
    const auto expect_rejected = [](LocalServerOptions options,
                                    const common::ErrorCode expected =
                                        common::ErrorCode::invalid_argument) {
        asio::io_context io_context;
        LocalServer server{io_context, std::make_shared<Dispatcher>(), std::move(options)};
        const auto started = server.start();
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error().code(), expected) << started.error();
    };

    expect_rejected({.socket_path = ""});
    expect_rejected({.socket_path = "relative.sock"});
    expect_rejected({.socket_path = std::string{"/tmp/bad\0path", 13U}});
    expect_rejected({.socket_path = "/" + std::string(sizeof(sockaddr_un::sun_path) - 1U, 'p')});
    expect_rejected({.socket_path = "/"});
    expect_rejected({.socket_path = temporary.path().string() + "/"});
    expect_rejected({.socket_path = temporary.path().string() + "//socket"});
    expect_rejected({.socket_path = temporary.path().string() + "/./socket"});
    expect_rejected({.socket_path = temporary.path().string() + "/../socket"});
    expect_rejected({.socket_path = temporary.socket_path(), .max_message_size = 0U});
    expect_rejected(
        {.socket_path = temporary.socket_path(), .max_message_size = kDefaultMaxFrameSize + 1U});
    expect_rejected({.socket_path = temporary.socket_path(), .request_timeout = 0ms});
    expect_rejected(
        {.socket_path = temporary.socket_path(), .request_timeout = kMaxLocalIpcTimeout + 1ms});
    expect_rejected({.socket_path = temporary.socket_path(), .max_connections = 0U});
    expect_rejected(
        {.socket_path = temporary.socket_path(), .max_connections = kMaxLocalConnections + 1U});
    expect_rejected({.socket_path = temporary.socket_path(), .socket_mode = 0666U});
    expect_rejected({.socket_path = temporary.socket_path(), .socket_mode = 01660U});

    const auto current_uid = static_cast<std::uint32_t>(::geteuid());
    const auto other_uid = current_uid == std::numeric_limits<std::uint32_t>::max()
                               ? current_uid - 1U
                               : current_uid + 1U;
    expect_rejected({.socket_path = temporary.socket_path(), .owner_uid = other_uid},
                    common::ErrorCode::permission_denied);

    asio::io_context io_context;
    LocalServer null_dispatcher{
        io_context, {}, {.socket_path = temporary.socket_path("null.sock")}};
    const auto null_start = null_dispatcher.start();
    ASSERT_FALSE(null_start);
    EXPECT_EQ(null_start.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcTransportTest, AcceptsExplicitOwnerGroupModeAndSupportsRestart) {
    TemporaryIpcDirectory temporary;
    asio::io_context io_context;
    const std::string socket_path = temporary.socket_path();
    LocalServer server{io_context,
                       std::make_shared<Dispatcher>(),
                       {.socket_path = socket_path,
                        .socket_mode = 0600U,
                        .owner_uid = static_cast<std::uint32_t>(::geteuid()),
                        .group_gid = static_cast<std::uint32_t>(::getegid())}};
    EXPECT_FALSE(server.is_running());
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());
    const auto duplicate = server.start();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    struct stat status{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &status), 0);
    EXPECT_EQ(static_cast<std::uint32_t>(status.st_mode) & 0777U, 0600U);
    server.stop();
    EXPECT_FALSE(server.is_running());
    server.stop();
    ASSERT_TRUE(server.start());
    server.stop();
}

TEST(IpcTransportTest, RejectsUntrustedParentComponentsAndSocketLockAttacks) {
    TemporaryIpcDirectory temporary;
    const auto start_at = [](const std::string& socket_path) {
        asio::io_context io_context;
        LocalServer server{
            io_context, std::make_shared<Dispatcher>(), {.socket_path = socket_path}};
        return server.start();
    };

    const auto regular_parent = temporary.path() / "regular-parent";
    {
        std::ofstream output{regular_parent};
        output << "not a directory";
    }
    const auto regular_result = start_at((regular_parent / "socket").string());
    ASSERT_FALSE(regular_result);
    EXPECT_EQ(regular_result.error().code(), common::ErrorCode::permission_denied);

    const auto writable_parent = temporary.path() / "writable-parent";
    ASSERT_TRUE(std::filesystem::create_directory(writable_parent));
    ASSERT_EQ(::chmod(writable_parent.c_str(), 0777), 0);
    const auto writable_result = start_at((writable_parent / "socket").string());
    ASSERT_FALSE(writable_result);
    EXPECT_EQ(writable_result.error().code(), common::ErrorCode::permission_denied);
    ASSERT_EQ(::chmod(writable_parent.c_str(), 0700), 0);

    const auto outer = temporary.path() / "outer";
    const auto inner = outer / "inner";
    ASSERT_TRUE(std::filesystem::create_directories(inner));
    ASSERT_EQ(::chmod(outer.c_str(), 0777), 0);
    const auto ancestor_result = start_at((inner / "socket").string());
    ASSERT_FALSE(ancestor_result);
    EXPECT_EQ(ancestor_result.error().code(), common::ErrorCode::permission_denied);
    ASSERT_EQ(::chmod(outer.c_str(), 0700), 0);

    const std::string symlink_socket = temporary.socket_path("symlink-lock.sock");
    ASSERT_EQ(::symlink("/dev/null", (symlink_socket + ".lock").c_str()), 0);
    const auto symlink_result = start_at(symlink_socket);
    ASSERT_FALSE(symlink_result);
    EXPECT_EQ(symlink_result.error().code(), common::ErrorCode::permission_denied);

    const std::string hardlink_socket = temporary.socket_path("hardlink-lock.sock");
    const auto hardlink_target = temporary.path() / "lock-target";
    {
        std::ofstream output{hardlink_target};
        output << "lock";
    }
    ASSERT_EQ(::chmod(hardlink_target.c_str(), 0600), 0);
    ASSERT_EQ(::link(hardlink_target.c_str(), (hardlink_socket + ".lock").c_str()), 0);
    const auto hardlink_result = start_at(hardlink_socket);
    ASSERT_FALSE(hardlink_result);
    EXPECT_EQ(hardlink_result.error().code(), common::ErrorCode::permission_denied);

    const std::string held_socket = temporary.socket_path("held-lock.sock");
    const std::string held_lock = held_socket + ".lock";
    const int descriptor = ::open(held_lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(descriptor, 0);
    ASSERT_EQ(::flock(descriptor, LOCK_EX | LOCK_NB), 0);
    const auto held_result = start_at(held_socket);
    ASSERT_FALSE(held_result);
    EXPECT_EQ(held_result.error().code(), common::ErrorCode::already_exists);
    ASSERT_EQ(::close(descriptor), 0);
}

TEST(IpcTransportTest, RejectsUnusualStaleSocketAndOversizedHandlerResponses) {
    TemporaryIpcDirectory temporary;
    const std::string unusual_path = temporary.socket_path("unusual.sock");
    asio::io_context stale_io_context;
    asio::local::stream_protocol::acceptor stale{
        stale_io_context, asio::local::stream_protocol::endpoint{unusual_path}};
    stale.close();
    struct stat status{};
    ASSERT_EQ(::lstat(unusual_path.c_str(), &status), 0);
    ASSERT_EQ(::chmod(unusual_path.c_str(), (status.st_mode & 0777) | S_ISVTX), 0);
    asio::io_context unusual_io_context;
    LocalServer unusual{
        unusual_io_context, std::make_shared<Dispatcher>(), {.socket_path = unusual_path}};
    const auto unusual_start = unusual.start();
    ASSERT_FALSE(unusual_start);
    EXPECT_EQ(unusual_start.error().code(), common::ErrorCode::already_exists);

    const std::string server_path = temporary.socket_path("response.sock");
    RunningServer server{LocalServerOptions{.socket_path = server_path, .max_message_size = 256U}};
    ASSERT_TRUE(server.register_handler("test.large", [](const Request&) -> common::Result<Json> {
        return Json{{"payload", std::string(512U, 'x')}};
    }));
    ASSERT_TRUE(server.register_handler("test.array", [](const Request&) -> common::Result<Json> {
        return Json::array({1, 2, 3});
    }));
    ASSERT_TRUE(server.start());
    LocalClient client{{.socket_path = server_path, .max_message_size = 1'024U}};
    const auto large = client.request(make_request(Json::object(), "test.large"));
    ASSERT_FALSE(large);
    EXPECT_EQ(large.error().code(), common::ErrorCode::ipc_error);
    const auto array = client.request(make_request(Json::object(), "test.array"));
    ASSERT_TRUE(array) << array.error();
    EXPECT_FALSE(array->ok());
    ASSERT_NE(array->error(), nullptr);
    EXPECT_EQ(array->error()->code(), common::ErrorCode::internal_error);
    const auto missing = client.request(make_request(Json::object(), "test.missing"));
    ASSERT_TRUE(missing) << missing.error();
    EXPECT_FALSE(missing->ok());
    EXPECT_EQ(missing->error()->code(), common::ErrorCode::not_found);
    server.stop();
}

TEST(IpcTransportTest, RejectsUnsafeClientOptionsBeforeConnecting) {
    const auto request = make_request();

    const auto relative =
        LocalClient{LocalClientOptions{.socket_path = "relative.sock"}}.request(request);
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code(), common::ErrorCode::invalid_argument);

    const auto zero_limit = LocalClient{
        LocalClientOptions{
            .socket_path = "/tmp/minitun-unused.sock",
            .max_message_size = 0U,
        }}.request(request);
    ASSERT_FALSE(zero_limit);
    EXPECT_EQ(zero_limit.error().code(), common::ErrorCode::invalid_argument);

    const auto zero_timeout = LocalClient{
        LocalClientOptions{
            .socket_path = "/tmp/minitun-unused.sock",
            .connect_timeout = 0ms,
        }}.request(request);
    ASSERT_FALSE(zero_timeout);
    EXPECT_EQ(zero_timeout.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcTransportTest, MapsEveryLocalSocketErrorClassWithoutLeakingDetails) {
    const auto expect_code = [](const asio::error_code& error, const common::ErrorCode expected) {
        const auto mapped = detail::socket_error(error, "test operation");
        EXPECT_EQ(mapped.code(), expected) << error.message();
        EXPECT_TRUE(mapped.message().starts_with("test operation failed"));
    };

    for (const auto error : {asio::error::access_denied, asio::error::no_permission}) {
        expect_code(error, common::ErrorCode::permission_denied);
    }
    expect_code(asio::error::address_in_use, common::ErrorCode::already_exists);
    for (const auto error :
         {asio::error::connection_refused, asio::error::connection_aborted,
          asio::error::connection_reset, asio::error::broken_pipe, asio::error::not_connected}) {
        expect_code(error, common::ErrorCode::connection_failed);
    }
    expect_code(asio::error_code{ENOENT, asio::error::get_system_category()},
                common::ErrorCode::connection_failed);
    expect_code(asio::error::timed_out, common::ErrorCode::connection_timeout);
    for (const auto error :
         {asio::error::no_descriptors, asio::error::no_buffer_space, asio::error::no_memory}) {
        expect_code(error, common::ErrorCode::resource_exhausted);
    }
    expect_code(asio::error::fault, common::ErrorCode::ipc_error);
    const auto no_error = detail::socket_error({}, "test operation");
    EXPECT_EQ(no_error.code(), common::ErrorCode::ipc_error);
    EXPECT_EQ(no_error.message(), "test operation failed");

    asio::io_context io_context;
    asio::steady_timer timer{io_context};
    detail::cancel_timer(timer);
}

} // namespace
} // namespace minitun::ipc
