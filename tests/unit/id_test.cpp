#include <array>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <minitun/common/id.hpp>

namespace minitun::common {
namespace {

struct KindExpectation final {
    IdKind kind;
    std::string_view name;
    std::string_view prefix;
};

constexpr std::array kKindExpectations{
    KindExpectation{IdKind::server, "server", "srv_"},
    KindExpectation{IdKind::tunnel, "tunnel", "tun_"},
    KindExpectation{IdKind::request, "request", "req_"},
    KindExpectation{IdKind::client, "client", "client_"},
    KindExpectation{IdKind::connection, "connection", "conn_"},
};

TEST(IdTest, DefinesStableNamesAndPrefixes) {
    for (const auto& expectation : kKindExpectations) {
        EXPECT_EQ(to_string(expectation.kind), expectation.name);
        EXPECT_EQ(id_prefix(expectation.kind), expectation.prefix);
    }

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_kind = static_cast<IdKind>(255);
    EXPECT_EQ(to_string(invalid_kind), "unknown");
    EXPECT_TRUE(id_prefix(invalid_kind).empty());
}

TEST(IdTest, GeneratesCanonicalCryptographicallyRandomIdentifiers) {
    for (const auto& expectation : kKindExpectations) {
        std::set<std::string> generated_values;
        for (std::size_t index = 0; index < 32; ++index) {
            auto generated = Id::generate(expectation.kind);
            ASSERT_TRUE(generated) << generated.error();

            EXPECT_EQ(generated->kind(), expectation.kind);
            EXPECT_EQ(generated->str().size(), expectation.prefix.size() + kIdHexCharacters);
            EXPECT_TRUE(generated->str().starts_with(expectation.prefix));
            EXPECT_EQ(generated->suffix().size(), kIdHexCharacters);
            EXPECT_TRUE(generated->suffix().find_first_not_of("0123456789abcdef") ==
                        std::string_view::npos);

            auto parsed = Id::parse(generated->str());
            ASSERT_TRUE(parsed) << parsed.error();
            EXPECT_EQ(*parsed, *generated);

            generated_values.insert(generated->str());
        }
        EXPECT_EQ(generated_values.size(), 32U);
    }
}

TEST(IdTest, ParsesEverySupportedKindAndStreamsItsCanonicalValue) {
    for (const auto& expectation : kKindExpectations) {
        const std::string text =
            std::string{expectation.prefix} + "0123456789abcdef0123456789abcdef";

        auto parsed = Id::parse(text, expectation.kind);

        ASSERT_TRUE(parsed) << parsed.error();
        EXPECT_EQ(parsed->kind(), expectation.kind);
        EXPECT_EQ(parsed->str(), text);
        EXPECT_EQ(parsed->suffix(), "0123456789abcdef0123456789abcdef");

        std::ostringstream stream;
        stream << *parsed;
        EXPECT_EQ(stream.str(), text);
    }
}

TEST(IdTest, StrictlyRejectsMalformedValues) {
    constexpr std::array malformed{
        "",
        "srv",
        "srv_",
        "server_0123456789abcdef0123456789abcdef",
        "srv_0123456789abcdef0123456789abcde",
        "srv_0123456789abcdef0123456789abcdef0",
        "srv_0123456789abcdef0123456789abcdeg",
        "srv_0123456789abcdef0123456789abcdeF",
        "srv_0123456789abcdef_123456789abcdef",
    };

    for (const std::string_view text : malformed) {
        const auto parsed = Id::parse(text);
        ASSERT_FALSE(parsed) << text;
        EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument) << text;
    }
}

TEST(IdTest, RejectsAValidIdOfTheWrongKind) {
    const auto parsed = Id::parse("srv_0123456789abcdef0123456789abcdef", IdKind::tunnel);

    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument);
}

TEST(IdTest, RejectsInvalidEnumValues) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_kind = static_cast<IdKind>(255);

    const auto generated = Id::generate(invalid_kind);
    ASSERT_FALSE(generated);
    EXPECT_EQ(generated.error().code(), ErrorCode::invalid_argument);

    const auto parsed = Id::parse("srv_0123456789abcdef0123456789abcdef", invalid_kind);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::common
