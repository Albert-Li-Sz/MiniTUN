#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/secure_string.hpp>

namespace minitun::common {
namespace {

template <typename T>
concept StreamInsertable = requires(std::ostream& stream, const T& value) { stream << value; };

static_assert(!std::is_copy_constructible_v<SecureString>);
static_assert(!std::is_copy_assignable_v<SecureString>);
static_assert(std::is_nothrow_move_constructible_v<SecureString>);
static_assert(std::is_nothrow_move_assignable_v<SecureString>);
static_assert(!std::is_convertible_v<SecureString, std::string>);
static_assert(!std::is_convertible_v<SecureString, std::string_view>);
static_assert(!StreamInsertable<SecureString>);
static_assert(std::is_same_v<decltype(std::declval<const SecureString&>().data()), const char*>);

TEST(SecureStringTest, PreservesBinaryContentThroughReadOnlyAccessors) {
    constexpr char value[] = {'t', 'o', '\0', 'k', 'e', 'n'};
    const SecureString secret{std::string_view{value, sizeof(value)}};

    EXPECT_EQ(secret.size(), sizeof(value));
    EXPECT_FALSE(secret.empty());
    EXPECT_EQ(secret.view(), std::string_view(value, sizeof(value)));
    EXPECT_EQ(secret.data()[2], '\0');
}

TEST(SecureStringTest, MovesExclusiveBufferAndEmptiesSource) {
    SecureString source{"move-only-secret"};
    const char* const original_buffer = source.data();

    SecureString destination{std::move(source)};

    EXPECT_TRUE(source.empty());
    EXPECT_EQ(source.size(), 0U);
    EXPECT_EQ(source.data(), nullptr);
    EXPECT_TRUE(source.view().empty());
    EXPECT_EQ(destination.data(), original_buffer);
    EXPECT_EQ(destination.view(), "move-only-secret");
}

TEST(SecureStringTest, MoveAssignmentReplacesExistingValueAndHandlesSelfMove) {
    SecureString source{"replacement"};
    const char* const source_buffer = source.data();
    SecureString destination{"value-to-erase"};

    destination = std::move(source);

    EXPECT_TRUE(source.empty());
    EXPECT_EQ(destination.data(), source_buffer);
    EXPECT_EQ(destination.view(), "replacement");

    SecureString* const alias = &destination;
    destination = std::move(*alias);
    EXPECT_EQ(destination.view(), "replacement");
}

TEST(SecureStringTest, ClearErasesValueAndIsIdempotent) {
    SecureString secret{"erase-me"};

    secret.clear();

    EXPECT_TRUE(secret.empty());
    EXPECT_EQ(secret.size(), 0U);
    EXPECT_EQ(secret.data(), nullptr);
    EXPECT_TRUE(secret.view().empty());

    secret.clear();
    EXPECT_TRUE(secret.empty());
}

TEST(SecureStringTest, ComparesEqualLengthValuesInConstantTimePrimitive) {
    constexpr char binary_value[] = {'a', '\0', 'b'};
    const SecureString first{std::string_view{binary_value, sizeof(binary_value)}};
    const SecureString same{std::string_view{binary_value, sizeof(binary_value)}};
    const SecureString different{std::string_view{"a\0c", 3U}};
    const SecureString shorter{"a"};

    EXPECT_TRUE(first.equals(first));
    EXPECT_TRUE(first.equals(same));
    EXPECT_FALSE(first.equals(different));
    EXPECT_FALSE(first.equals(shorter));
}

TEST(SecureStringTest, HandlesEmptyValuesSafely) {
    const SecureString default_empty;
    const SecureString explicit_empty{std::string_view{}};
    const SecureString non_empty{"x"};

    EXPECT_TRUE(default_empty.empty());
    EXPECT_EQ(default_empty.data(), nullptr);
    EXPECT_TRUE(default_empty.view().empty());
    EXPECT_TRUE(default_empty.equals(explicit_empty));
    EXPECT_FALSE(default_empty.equals(non_empty));
    EXPECT_FALSE(non_empty.equals(default_empty));
}

} // namespace
} // namespace minitun::common
