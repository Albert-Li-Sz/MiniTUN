#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <minitun/admin/server.hpp>
#include <minitun/common/error.hpp>

namespace minitun::admin {
namespace {

[[nodiscard]] std::uint16_t available_port() {
    asio::io_context io_context;
    asio::ip::tcp::acceptor probe{io_context, asio::ip::tcp::endpoint{asio::ip::tcp::v4(), 0U}};
    return probe.local_endpoint().port();
}

class PrivateTokenFile final {
  public:
    explicit PrivateTokenFile(const std::string_view contents = "correct-test-token\n",
                              const mode_t mode = S_IRUSR | S_IWUSR) {
        std::array<char, 64U> pattern{};
        constexpr std::string_view prefix{"/tmp/minitun-admin-token.XXXXXX"};
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        descriptor_ = ::mkstemp(pattern.data());
        if (descriptor_ >= 0) {
            path_ = pattern.data();
            static_cast<void>(::fchmod(descriptor_, mode));
            const auto written = ::write(descriptor_, contents.data(), contents.size());
            EXPECT_EQ(written, static_cast<ssize_t>(contents.size()));
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
    }

    ~PrivateTokenFile() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

  private:
    int descriptor_{-1};
    std::string path_;
};

[[nodiscard]] std::string request(const std::uint16_t port, const std::string_view message) {
    asio::io_context io_context;
    asio::ip::tcp::socket socket{io_context};
    asio::error_code error;
    socket.connect({asio::ip::make_address("127.0.0.1"), port}, error);
    EXPECT_FALSE(error) << error.message();
    if (error) {
        return {};
    }
    asio::write(socket, asio::buffer(message), error);
    EXPECT_FALSE(error) << error.message();
    if (error) {
        return {};
    }
    std::string response;
    std::array<char, 1'024U> bytes{};
    for (;;) {
        const std::size_t count = socket.read_some(asio::buffer(bytes), error);
        response.append(bytes.data(), count);
        if (error == asio::error::eof) {
            break;
        }
        EXPECT_FALSE(error) << error.message();
        if (error) {
            break;
        }
    }
    return response;
}

class RunningAdminServer final {
  public:
    RunningAdminServer(ServerOptions options, Providers providers) {
        auto created = Server::create(io_context_, std::move(options), std::move(providers));
        EXPECT_TRUE(created) << created.error();
        if (!created) {
            return;
        }
        server_ = std::move(*created);
        auto started = server_->start();
        EXPECT_TRUE(started) << started.error();
        if (!started) {
            server_.reset();
            return;
        }
        port_ = server_->listening_port();
        worker_ = std::thread{[this] { io_context_.run(); }};
    }

    ~RunningAdminServer() {
        if (server_ != nullptr) {
            server_->stop();
        }
        io_context_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  private:
    asio::io_context io_context_;
    std::unique_ptr<Server> server_;
    std::uint16_t port_{0U};
    std::thread worker_;
};

TEST(AdminServerTest, ServesBoundedHealthReadinessAndMetricsSurface) {
    std::atomic_bool ready{false};
    RunningAdminServer server{
        {.listen_endpoint = "127.0.0.1:" + std::to_string(available_port()), .token_file = {}},
        {
            .healthy = [] { return true; },
            .ready = [&ready] { return ready.load(); },
            .metrics = [] { return std::string{"minitun_test_metric 7\n"}; },
        }};
    ASSERT_NE(server.port(), 0U);

    const auto health = request(server.port(), "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(health.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(health.ends_with("ok\n"));

    const auto unavailable =
        request(server.port(), "HEAD /readyz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(unavailable.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_TRUE(unavailable.ends_with("\r\n\r\n"));
    EXPECT_EQ(unavailable.find("not ready\n"), std::string::npos);

    ready.store(true);
    const auto readiness =
        request(server.port(), "GET /readyz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(readiness.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(readiness.ends_with("ready\n"));

    const auto metrics = request(server.port(), "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(metrics.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(metrics.ends_with("minitun_test_metric 7\n"));

    const auto metrics_head =
        request(server.port(), "HEAD /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(metrics_head.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));

    const auto post = request(server.port(), "POST /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(post.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
}

TEST(AdminServerTest, RejectsMissingAndWrongBearerTokensOnWildcardListener) {
    PrivateTokenFile token;
    ASSERT_FALSE(token.path().empty());
    RunningAdminServer server{{.listen_endpoint = "0.0.0.0:" + std::to_string(available_port()),
                               .token_file = token.path()},
                              {
                                  .healthy = [] { return true; },
                                  .ready = [] { return true; },
                                  .metrics = [] { return std::string{}; },
                              }};
    ASSERT_NE(server.port(), 0U);

    const auto missing = request(server.port(), "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(missing.starts_with("HTTP/1.1 401 Unauthorized\r\n"));
    const auto wrong = request(server.port(), "GET /healthz HTTP/1.1\r\nHost: localhost\r\n"
                                              "Authorization: Bearer wrong\r\n\r\n");
    EXPECT_TRUE(wrong.starts_with("HTTP/1.1 401 Unauthorized\r\n"));
    const auto accepted =
        request(server.port(), "GET /healthz HTTP/1.1\r\nHost: localhost\r\n"
                               "Authorization: Bearer correct-test-token\r\n\r\n");
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST(AdminServerTest, NonLoopbackListenerWithoutTokenIsRejected) {
    asio::io_context io_context;
    auto created = Server::create(
        io_context,
        {.listen_endpoint = "0.0.0.0:" + std::to_string(available_port()), .token_file = {}},
        {.healthy = [] { return true; },
         .ready = [] { return true; },
         .metrics = [] { return std::string{}; }});
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().code(), common::ErrorCode::permission_denied);
}

TEST(AdminServerTest, RejectsEveryInvalidListenerOptionBeforeBinding) {
    asio::io_context io_context;
    const Providers providers{};
    const auto expect_invalid = [&io_context, &providers](ServerOptions options) {
        const auto created = Server::create(io_context, std::move(options), providers);
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument);
    };

    expect_invalid({.listen_endpoint = ""});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .max_connections = 0U});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .max_connections = 1'025U});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .max_header_bytes = 1'023U});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .max_header_bytes = 64U * 1'024U + 1U});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .timeout = std::chrono::seconds::zero()});
    expect_invalid({.listen_endpoint = "127.0.0.1:0", .timeout = std::chrono::seconds{61}});
    expect_invalid({.listen_endpoint = "missing-port"});
    expect_invalid({.listen_endpoint = "localhost:2333"});
}

TEST(AdminServerTest, RejectsUnsafeOrMalformedTokenFiles) {
    asio::io_context io_context;
    const auto expect_rejected = [&io_context](std::string path) {
        const auto created = Server::create(
            io_context, {.listen_endpoint = "127.0.0.1:0", .token_file = std::move(path)}, {});
        ASSERT_FALSE(created);
        EXPECT_TRUE(created.error().code() == common::ErrorCode::invalid_argument ||
                    created.error().code() == common::ErrorCode::permission_denied);
    };

    expect_rejected("/tmp/minitun-token-that-does-not-exist");
    expect_rejected(std::string(4'097U, 'p'));
    expect_rejected(std::string{"bad\0path", 8U});
    expect_rejected("/tmp");

    PrivateTokenFile empty{""};
    expect_rejected(empty.path());
    PrivateTokenFile only_newlines{"\r\n"};
    expect_rejected(only_newlines.path());
    PrivateTokenFile embedded_nul{std::string_view{"abc\0def", 7U}};
    expect_rejected(embedded_nul.path());
    PrivateTokenFile public_mode{"secret", S_IRUSR | S_IWUSR | S_IRGRP};
    expect_rejected(public_mode.path());
    PrivateTokenFile oversized{std::string(4'097U, 's')};
    expect_rejected(oversized.path());

    PrivateTokenFile hardlinked{"secret"};
    const std::string second_link = hardlinked.path() + ".link";
    ASSERT_EQ(::link(hardlinked.path().c_str(), second_link.c_str()), 0);
    expect_rejected(hardlinked.path());
    std::error_code ignored;
    std::filesystem::remove(second_link, ignored);

    PrivateTokenFile target{"secret"};
    const std::string symlink = target.path() + ".symlink";
    ASSERT_EQ(::symlink(target.path().c_str(), symlink.c_str()), 0);
    expect_rejected(symlink);
    std::filesystem::remove(symlink, ignored);
}

TEST(AdminServerTest, StrictlyRejectsMalformedHttpAndUnknownRoutes) {
    RunningAdminServer server{
        {.listen_endpoint = "127.0.0.1:" + std::to_string(available_port()), .token_file = {}},
        {.healthy = [] { return true; },
         .ready = [] { return true; },
         .metrics = [] { return std::string{}; }}};
    ASSERT_NE(server.port(), 0U);

    constexpr std::array<std::string_view, 12> bad_requests{
        "\r\n\r\n",
        "GET\r\n\r\n",
        "GET  /healthz HTTP/1.1\r\n\r\n",
        "GET /healthz HTTP/1.0\r\n\r\n",
        "GET healthz HTTP/1.1\r\n\r\n",
        "GET /healthz?probe=1 HTTP/1.1\r\n\r\n",
        "GET /healthz HTTP/1.1\r\ninvalid-header\r\n\r\n",
        "GET /healthz HTTP/1.1\r\n:value\r\n\r\n",
        "GET /healthz HTTP/1.1\r\nContent-Length: 1\r\n\r\n",
        "GET /healthz HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET /healthz HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /healthz HTTP/1.1\r\nAuthorization: one\r\nAuthorization: two\r\n\r\n",
    };
    for (const auto bad_request : bad_requests) {
        const auto result = request(server.port(), bad_request);
        EXPECT_TRUE(result.starts_with("HTTP/1.1 400 Bad Request\r\n")) << bad_request;
    }

    std::string too_many_headers{"GET /healthz HTTP/1.1\r\n"};
    for (std::size_t index = 0U; index < 65U; ++index) {
        too_many_headers.append("X-Test-").append(std::to_string(index)).append(": value\r\n");
    }
    too_many_headers.append("\r\n");
    EXPECT_TRUE(
        request(server.port(), too_many_headers).starts_with("HTTP/1.1 400 Bad Request\r\n"));

    const auto missing = request(server.port(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(missing.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(missing.ends_with("not found\n"));
    const auto missing_head =
        request(server.port(), "HEAD /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(missing_head.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(missing_head.ends_with("\r\n\r\n"));
}

TEST(AdminServerTest, ContainsProviderFailuresAndMissingProviders) {
    RunningAdminServer throwing{
        {.listen_endpoint = "127.0.0.1:" + std::to_string(available_port()), .token_file = {}},
        {.healthy = []() -> bool { throw std::runtime_error("health failure"); },
         .ready = []() -> bool { throw std::runtime_error("ready failure"); },
         .metrics = []() -> std::string { throw std::runtime_error("metrics failure"); }}};
    ASSERT_NE(throwing.port(), 0U);
    EXPECT_TRUE(request(throwing.port(), "GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n")
                    .starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_TRUE(request(throwing.port(), "GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n")
                    .starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_TRUE(request(throwing.port(), "GET /metrics HTTP/1.1\r\nHost: test\r\n\r\n")
                    .starts_with("HTTP/1.1 500 Internal Server Error\r\n"));

    RunningAdminServer missing{
        {.listen_endpoint = "127.0.0.1:" + std::to_string(available_port()), .token_file = {}}, {}};
    ASSERT_NE(missing.port(), 0U);
    EXPECT_TRUE(request(missing.port(), "HEAD /healthz HTTP/1.1\r\nHost: test\r\n\r\n")
                    .starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_TRUE(request(missing.port(), "GET /metrics HTTP/1.1\r\nHost: test\r\n\r\n")
                    .starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST(AdminServerTest, EnforcesHeaderTimeoutAndSizeBounds) {
    RunningAdminServer server{{.listen_endpoint = "127.0.0.1:" + std::to_string(available_port()),
                               .token_file = {},
                               .max_header_bytes = 1'024U,
                               .timeout = std::chrono::seconds{1}},
                              {}};
    ASSERT_NE(server.port(), 0U);

    std::string oversized{"GET /healthz HTTP/1.1\r\nX-Large: "};
    oversized.append(1'100U, 'x').append("\r\n\r\n");
    EXPECT_TRUE(request(server.port(), oversized)
                    .starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));

    asio::io_context io_context;
    asio::ip::tcp::socket socket{io_context};
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    asio::write(socket, asio::buffer("GET /healthz HTTP/1.1\r\nHost: incomplete"));
    std::array<char, 64U> bytes{};
    asio::error_code error;
    const auto count = socket.read_some(asio::buffer(bytes), error);
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(error, asio::error::eof);
}

TEST(AdminServerTest, StartStopAreIdempotentAndPortConflictsAreContained) {
    asio::io_context io_context;
    const auto port = available_port();
    auto first =
        Server::create(io_context, {.listen_endpoint = "127.0.0.1:" + std::to_string(port)}, {});
    ASSERT_TRUE(first) << first.error();
    ASSERT_TRUE((*first)->start());
    EXPECT_EQ((*first)->listening_port(), port);
    const auto duplicate_start = (*first)->start();
    ASSERT_FALSE(duplicate_start);
    EXPECT_EQ(duplicate_start.error().code(), common::ErrorCode::already_exists);

    auto second =
        Server::create(io_context, {.listen_endpoint = "127.0.0.1:" + std::to_string(port)}, {});
    ASSERT_TRUE(second) << second.error();
    const auto conflicting = (*second)->start();
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().code(), common::ErrorCode::connection_failed);
    EXPECT_EQ((*second)->listening_port(), 0U);
    (*second)->stop();

    (*first)->stop();
    (*first)->stop();
    EXPECT_EQ((*first)->listening_port(), 0U);
}

} // namespace
} // namespace minitun::admin
