#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>

namespace minitun::common {
namespace {

TEST(EndpointTest, ParsesAndCanonicalizesDomainName) {
    const auto endpoint = Endpoint::parse("Tunnel.Example.COM:2333");

    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint->host(), "tunnel.example.com");
    EXPECT_EQ(endpoint->port(), 2333);
    EXPECT_EQ(endpoint->kind(), EndpointKind::domain_name);
    EXPECT_TRUE(endpoint->is_domain());
    EXPECT_FALSE(endpoint->is_ipv4());
    EXPECT_FALSE(endpoint->is_ipv6());
    EXPECT_EQ(endpoint->to_string(), "tunnel.example.com:2333");
}

TEST(EndpointTest, AcceptsSingleLabelAndAbsoluteDomainNames) {
    const auto single_label = Endpoint::parse("localhost:1");
    const auto absolute = Endpoint::parse("Example.COM.:65535");

    ASSERT_TRUE(single_label);
    EXPECT_EQ(single_label->host(), "localhost");
    ASSERT_TRUE(absolute);
    EXPECT_EQ(absolute->host(), "example.com.");
    EXPECT_EQ(absolute->to_string(), "example.com.:65535");
}

TEST(EndpointTest, ParsesAndCanonicalizesIpv4) {
    const auto endpoint = Endpoint::parse("192.0.2.10:443");

    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint->host(), "192.0.2.10");
    EXPECT_EQ(endpoint->port(), 443);
    EXPECT_EQ(endpoint->kind(), EndpointKind::ipv4);
    EXPECT_TRUE(endpoint->is_ipv4());
    EXPECT_EQ(endpoint->to_string(), "192.0.2.10:443");
}

TEST(EndpointTest, ParsesIpv6AndRestoresRequiredBrackets) {
    const auto endpoint = Endpoint::parse("[2001:0db8:0:0:0:0:0:1]:8443");

    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint->host(), "2001:db8::1");
    EXPECT_EQ(endpoint->port(), 8443);
    EXPECT_EQ(endpoint->kind(), EndpointKind::ipv6);
    EXPECT_TRUE(endpoint->is_ipv6());
    EXPECT_EQ(endpoint->to_string(), "[2001:db8::1]:8443");
}

TEST(EndpointTest, CanonicalOutputRoundTripsForEveryHostKind) {
    constexpr std::array<std::string_view, 3> inputs{
        "EXAMPLE.net:1234",
        "203.0.113.7:65535",
        "[2001:db8::abcd]:1",
    };

    for (const std::string_view input : inputs) {
        const auto parsed = Endpoint::parse(input);
        ASSERT_TRUE(parsed) << input;

        const auto reparsed = Endpoint::parse(parsed->to_string());
        ASSERT_TRUE(reparsed) << input;
        EXPECT_EQ(*reparsed, *parsed) << input;
    }
}

TEST(EndpointTest, RejectsWhitespaceAnywhere) {
    constexpr std::array<std::string_view, 8> invalid{
        " example.com:80", "example.com:80 ",  "example .com:80", "example.com :80",
        "example.com: 80", "example.com:\t80", "[::1] :80",       "[::1]:80\n",
    };

    for (const std::string_view input : invalid) {
        const auto parsed = Endpoint::parse(input);
        ASSERT_FALSE(parsed) << input;
        EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument) << input;
    }
}

TEST(EndpointTest, RejectsMissingOrAmbiguousHostPortSyntax) {
    constexpr std::array<std::string_view, 12> invalid{
        "",       ":80",   "example.com", "example.com:",  "example.com:80:90", "2001:db8::1:443",
        "[]:443", "[::1]", "[::1]443",    "[::1]:443:444", "[127.0.0.1]:443",   "example.com]:443",
    };

    for (const std::string_view input : invalid) {
        EXPECT_FALSE(Endpoint::parse(input)) << input;
    }
}

TEST(EndpointTest, RejectsInvalidAndAmbiguousPortNumbers) {
    constexpr std::array<std::string_view, 11> invalid{
        "example.com:0",
        "example.com:65536",
        "example.com:999999999999999999999999",
        "example.com:+80",
        "example.com:-80",
        "example.com:0x50",
        "example.com:8O",
        "[::1]:0",
        "[::1]:65536",
        "127.0.0.1:+1",
        "example.com:000080",
    };

    for (const std::string_view input : invalid) {
        EXPECT_FALSE(Endpoint::parse(input)) << input;
    }
}

TEST(EndpointTest, RejectsInputBeyondTheBoundedEndpointLength) {
    const std::string oversized_host(256, 'a');

    const auto parsed = Endpoint::parse(oversized_host + ":443");

    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument);
}

TEST(EndpointTest, RejectsMalformedDomains) {
    const std::string label64(64, 'a');
    const std::string domain253 = std::string(63, 'a') + '.' + std::string(63, 'b') + '.' +
                                  std::string(63, 'c') + '.' + std::string(61, 'd');
    const std::string domain254 = domain253 + 'd';

    ASSERT_TRUE(Endpoint::parse(domain253 + ":80"));

    const std::array<std::string, 11> invalid{
        "-example.com:80",
        "example-.com:80",
        "example..com:80",
        ".example.com:80",
        "example.com..:80",
        "under_score.example:80",
        "example!.com:80",
        label64 + ".example:80",
        domain254 + ":80",
        std::string{"\xC3\xA9xample.com:80"},
        ".:80",
    };

    for (const std::string& input : invalid) {
        EXPECT_FALSE(Endpoint::parse(input)) << input;
    }
}

TEST(EndpointTest, RejectsNumericDottedIpv4Lookalikes) {
    constexpr std::array<std::string_view, 14> invalid{
        "256.1.1.1:80",  "999.999.999.999:80",  "192.0.2:80",      "192.0.2.1.5:80",
        "192..2.1:80",   "01.2.3.4:80",         "1.2.3.4.:80",     "1...4:80",
        "127:80",        "2130706433:80",       "017700000001:80", "0x7f000001:80",
        "0X7F000001:80", "0x7f.0x0.0x0.0x1:80",
    };

    for (const std::string_view input : invalid) {
        EXPECT_FALSE(Endpoint::parse(input)) << input;
    }
}

TEST(EndpointTest, RejectsMalformedIpv6AndScopeIdentifiers) {
    constexpr std::array<std::string_view, 7> invalid{
        "[2001:db8:::1]:443",       "[2001:db8::gg]:443", "[2001:db8::1:443",
        "[2001:db8::1]]:443",       "[fe80::1%eth0]:443", "[v1.example]:443",
        "[::ffff:192.0.2.999]:443",
    };

    for (const std::string_view input : invalid) {
        EXPECT_FALSE(Endpoint::parse(input)) << input;
    }
}

} // namespace
} // namespace minitun::common
