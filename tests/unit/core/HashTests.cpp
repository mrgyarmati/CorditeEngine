#include "cordite/core/Hash.h"

#include "support/TestSupport.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace cordite;

namespace {

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

} // namespace

TEST_SUITE("Hash") {
    TEST_CASE("fnv1a64 matches the published test vectors") {
        static_assert(fnv1a64("") == 0xcbf29ce484222325ull);
        static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cull);
        static_assert(fnv1a64("foobar") == 0x85944171f73967e8ull);
        CHECK(fnv1a64("foobar") == 0x85944171f73967e8ull);
    }

    TEST_CASE("hashString is deterministic and seed-sensitive") {
        CHECK(hashString("cordite") == hashString("cordite"));
        CHECK(hashString("cordite") != hashString("Cordite"));
        CHECK(hashString("cordite", 1) != hashString("cordite", 2));
        CHECK(hashString("cordite") == hashBytes(bytesOf("cordite")));
    }

    TEST_CASE("hashBytes128 matches the XXH3-128 reference value for empty input") {
        // From the xxHash specification: XXH3_128bits("") with seed 0.
        const Hash128 digest = hashBytes128({});
        CHECK(digest.toHex() == "99aa06d3014798d86001c324468d497f");
    }

    TEST_CASE("Hash128 converts to and from hex") {
        const Hash128 original{0x0123456789abcdefull, 0xfedcba9876543210ull};
        const std::string hex = original.toHex();
        CHECK(hex == "0123456789abcdeffedcba9876543210");

        const std::optional<Hash128> parsed = Hash128::fromHex(hex);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == original);
    }

    TEST_CASE("Hash128::fromHex rejects malformed input") {
        CHECK_FALSE(Hash128::fromHex("").has_value());
        CHECK_FALSE(Hash128::fromHex("0123").has_value());
        CHECK_FALSE(Hash128::fromHex("0123456789abcdeffedcba987654321g").has_value());
        CHECK_FALSE(Hash128::fromHex("0123456789ABCDEFFEDCBA9876543210").has_value());
        CHECK_FALSE(Hash128::fromHex("0123456789abcdeffedcba98765432100").has_value());
    }

    TEST_CASE("ContentHasher over raw bytes equals the one-shot hash") {
        const std::string text = "The quick brown fox jumps over the lazy dog";

        ContentHasher hasher;
        hasher.update(bytesOf(text.substr(0, 10)));
        hasher.update(bytesOf(text.substr(10)));

        CHECK(hasher.finish() == hashBytes128(bytesOf(text)));
    }

    TEST_CASE("ContentHasher string updates are length-prefixed") {
        ContentHasher first;
        first.update(std::string_view("ab")).update(std::string_view("c"));

        ContentHasher second;
        second.update(std::string_view("a")).update(std::string_view("bc"));

        CHECK(first.finish() != second.finish());
    }

    TEST_CASE("ContentHasher is order-sensitive and repeatable") {
        const uint32_t version = 7;

        ContentHasher a;
        a.update(std::string_view("shader")).updateValue(version);
        ContentHasher b;
        b.update(std::string_view("shader")).updateValue(version);
        ContentHasher c;
        c.updateValue(version).update(std::string_view("shader"));

        CHECK(a.finish() == b.finish());
        CHECK(a.finish() != c.finish());
        // finish() does not consume the state.
        CHECK(a.finish() == a.finish());
    }

    TEST_CASE("hashCombine depends on order") {
        const uint64_t ab = hashCombine(hashCombine(0, 1), 2);
        const uint64_t ba = hashCombine(hashCombine(0, 2), 1);
        CHECK(ab != ba);
    }

    TEST_CASE("hashString spreads sequential keys") {
        std::set<uint64_t> seen;
        for (int index = 0; index < 10000; ++index) {
            seen.insert(hashString("key" + std::to_string(index)));
        }
        CHECK(seen.size() == 10000);
    }
}
