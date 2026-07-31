#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/ipc/dispatcher.hpp>

namespace minitun::ipc {
namespace {

[[nodiscard]] common::Id make_request_id() {
    return common::Id::parse("req_0123456789abcdef0123456789abcdef", common::IdKind::request)
        .value();
}

[[nodiscard]] Request make_request(std::string method = "daemon.status") {
    return Request{kProtocolVersion, make_request_id(), std::move(method), Json::object()};
}

TEST(IpcDispatcherTest, DispatchesARegisteredMethodAndPreservesCorrelationId) {
    Dispatcher dispatcher;
    const auto registered = dispatcher.register_handler(
        "daemon.status", [](const Request& request) -> common::Result<Json> {
            return Json{{"method", request.method}, {"state", "running"}};
        });
    ASSERT_TRUE(registered) << registered.error();
    EXPECT_EQ(dispatcher.size(), 1U);

    const Request request = make_request();
    const Response response = dispatcher.dispatch(request);

    EXPECT_TRUE(response.ok());
    EXPECT_EQ(response.request_id(), request.request_id);
    ASSERT_NE(response.result(), nullptr);
    EXPECT_EQ(*response.result(), (Json{{"method", "daemon.status"}, {"state", "running"}}));
    EXPECT_EQ(response.error(), nullptr);
}

TEST(IpcDispatcherTest, PropagatesAHandlerErrorWithoutChangingItsCode) {
    Dispatcher dispatcher;
    ASSERT_TRUE(dispatcher.register_handler("tun.add", [](const Request&) -> common::Result<Json> {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "remote port is invalid");
    }));

    const Response response = dispatcher.dispatch(make_request("tun.add"));

    EXPECT_FALSE(response.ok());
    ASSERT_NE(response.error(), nullptr);
    EXPECT_EQ(response.error()->code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(response.error()->message(), "remote port is invalid");
}

TEST(IpcDispatcherTest, RejectsUnknownMalformedAndUnsupportedRequests) {
    Dispatcher dispatcher;

    const Response unknown = dispatcher.dispatch(make_request("server.list"));
    ASSERT_FALSE(unknown.ok());
    ASSERT_NE(unknown.error(), nullptr);
    EXPECT_EQ(unknown.error()->code(), common::ErrorCode::not_found);

    Request malformed = make_request();
    malformed.method = "Daemon status";
    const Response invalid_method = dispatcher.dispatch(malformed);
    ASSERT_FALSE(invalid_method.ok());
    ASSERT_NE(invalid_method.error(), nullptr);
    EXPECT_EQ(invalid_method.error()->code(), common::ErrorCode::invalid_argument);

    Request future = make_request();
    future.version = kProtocolVersion + 1U;
    const Response unsupported = dispatcher.dispatch(future);
    ASSERT_FALSE(unsupported.ok());
    ASSERT_NE(unsupported.error(), nullptr);
    EXPECT_EQ(unsupported.error()->code(), common::ErrorCode::unsupported_version);
}

TEST(IpcDispatcherTest, RejectsInvalidRegistrationsAndKeepsTheOriginalDuplicate) {
    Dispatcher dispatcher;
    const auto malformed = dispatcher.register_handler(
        "daemon..status", [](const Request&) -> common::Result<Json> { return Json::object(); });
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), common::ErrorCode::invalid_argument);

    const auto empty = dispatcher.register_handler("daemon.empty", MethodHandler{});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), common::ErrorCode::invalid_argument);

    ASSERT_TRUE(
        dispatcher.register_handler("daemon.status", [](const Request&) -> common::Result<Json> {
            return Json{{"handler", "original"}};
        }));
    const auto duplicate =
        dispatcher.register_handler("daemon.status", [](const Request&) -> common::Result<Json> {
            return Json{{"handler", "replacement"}};
        });
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    EXPECT_EQ(dispatcher.size(), 1U);

    const Response response = dispatcher.dispatch(make_request());
    ASSERT_TRUE(response.ok());
    ASSERT_NE(response.result(), nullptr);
    EXPECT_EQ(response.result()->at("handler"), "original");
}

TEST(IpcDispatcherTest, UnregistersMethodsWithStableErrors) {
    Dispatcher dispatcher;
    ASSERT_TRUE(dispatcher.register_handler(
        "daemon.status", [](const Request&) -> common::Result<Json> { return Json::object(); }));

    EXPECT_TRUE(dispatcher.unregister_handler("daemon.status"));
    EXPECT_EQ(dispatcher.size(), 0U);

    const auto missing = dispatcher.unregister_handler("daemon.status");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);

    const auto malformed = dispatcher.unregister_handler("daemon..status");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), common::ErrorCode::invalid_argument);

    const Response response = dispatcher.dispatch(make_request());
    ASSERT_FALSE(response.ok());
    ASSERT_NE(response.error(), nullptr);
    EXPECT_EQ(response.error()->code(), common::ErrorCode::not_found);
}

TEST(IpcDispatcherTest, ConvertsInvalidHandlerResultsToInternalError) {
    Dispatcher dispatcher;
    ASSERT_TRUE(
        dispatcher.register_handler("daemon.status", [](const Request&) -> common::Result<Json> {
            return Json::array({"not", "an", "object"});
        }));

    const Response response = dispatcher.dispatch(make_request());

    ASSERT_FALSE(response.ok());
    ASSERT_NE(response.error(), nullptr);
    EXPECT_EQ(response.error()->code(), common::ErrorCode::internal_error);
}

TEST(IpcDispatcherTest, ContainsStandardAndNonStandardHandlerExceptions) {
    Dispatcher dispatcher;
    ASSERT_TRUE(
        dispatcher.register_handler("daemon.standard", [](const Request&) -> common::Result<Json> {
            throw std::runtime_error{"secret handler detail"};
        }));
    ASSERT_TRUE(dispatcher.register_handler(
        "daemon.nonstandard", [](const Request&) -> common::Result<Json> { throw 7; }));

    for (const std::string method : {"daemon.standard", "daemon.nonstandard"}) {
        const Response response = dispatcher.dispatch(make_request(method));
        ASSERT_FALSE(response.ok());
        ASSERT_NE(response.error(), nullptr);
        EXPECT_EQ(response.error()->code(), common::ErrorCode::internal_error);
        EXPECT_EQ(response.error()->message(), "IPC method handler failed");
        EXPECT_EQ(response.error()->message().find("secret"), std::string::npos);
    }
}

TEST(IpcDispatcherTest, ReleasesTheRegistryLockBeforeInvokingAHandler) {
    Dispatcher dispatcher;
    ASSERT_TRUE(dispatcher.register_handler(
        "daemon.install", [&dispatcher](const Request&) -> common::Result<Json> {
            auto registered = dispatcher.register_handler(
                "daemon.dynamic",
                [](const Request&) -> common::Result<Json> { return Json{{"ready", true}}; });
            if (!registered) {
                return common::Result<Json>::failure(registered.error());
            }
            return Json{{"installed", true}};
        }));

    const Response installed = dispatcher.dispatch(make_request("daemon.install"));
    ASSERT_TRUE(installed.ok());
    EXPECT_EQ(dispatcher.size(), 2U);

    const Response dynamic = dispatcher.dispatch(make_request("daemon.dynamic"));
    ASSERT_TRUE(dynamic.ok());
    ASSERT_NE(dynamic.result(), nullptr);
    EXPECT_EQ(dynamic.result()->at("ready"), true);
}

TEST(IpcDispatcherTest, DispatchesTheSameHandlerConcurrently) {
    Dispatcher dispatcher;
    std::atomic_size_t calls{0U};
    ASSERT_TRUE(dispatcher.register_handler("daemon.status",
                                            [&calls](const Request&) -> common::Result<Json> {
                                                calls.fetch_add(1U, std::memory_order_relaxed);
                                                return Json{{"state", "running"}};
                                            }));

    constexpr std::size_t kThreadCount = 8U;
    constexpr std::size_t kCallsPerThread = 250U;
    std::atomic_size_t failures{0U};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        threads.emplace_back([&dispatcher, &failures]() {
            const Request request = make_request();
            for (std::size_t call = 0; call < kCallsPerThread; ++call) {
                const Response response = dispatcher.dispatch(request);
                if (!response.ok() || response.result() == nullptr ||
                    response.result()->value("state", "") != "running") {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), kThreadCount * kCallsPerThread);
}

TEST(IpcDispatcherTest, SupportsConcurrentRegistryUpdates) {
    Dispatcher dispatcher;
    constexpr std::size_t kThreadCount = 16U;
    std::atomic_size_t failures{0U};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&dispatcher, &failures, index]() {
            const std::string method = "method.m" + std::to_string(index);
            const auto registered = dispatcher.register_handler(
                method, [](const Request&) -> common::Result<Json> { return Json{{"ok", true}}; });
            if (!registered) {
                failures.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
            const Response response = dispatcher.dispatch(make_request(method));
            if (!response.ok()) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(dispatcher.size(), kThreadCount);
}

} // namespace
} // namespace minitun::ipc
