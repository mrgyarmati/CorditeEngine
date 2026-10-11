#include "cordite/core/Uuid.h"

#include "support/TestSupport.h"

#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace cordite;

TEST_SUITE("Uuid") {
    TEST_CASE("the default Uuid is nil") {
        const Uuid id;
        CHECK(id.isNil());
        CHECK(id.toString() == "00000000-0000-0000-0000-000000000000");
    }

    TEST_CASE("generate produces version 4, RFC variant identifiers") {
        for (int index = 0; index < 100; ++index) {
            const Uuid id = Uuid::generate();
            CHECK_FALSE(id.isNil());
            CHECK(((id.high() >> 12) & 0xf) == 4);
            CHECK((id.low() >> 62) == 0b10);

            const std::string text = id.toString();
            CHECK(text.size() == 36);
            CHECK(text[14] == '4');
        }
    }

    TEST_CASE("generated identifiers do not collide") {
        std::unordered_set<Uuid> seen;
        for (int index = 0; index < 20000; ++index) {
            CHECK(seen.insert(Uuid::generate()).second);
        }
    }

    TEST_CASE("generate is safe to call from several threads") {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 2000;
        std::vector<std::vector<Uuid>> results(kThreads);
        std::vector<std::thread> threads;
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&results, thread] {
                for (int index = 0; index < kPerThread; ++index) {
                    results[static_cast<size_t>(thread)].push_back(Uuid::generate());
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        std::set<Uuid> all;
        for (const std::vector<Uuid>& list : results) {
            all.insert(list.begin(), list.end());
        }
        CHECK(all.size() == static_cast<size_t>(kThreads * kPerThread));
    }

    TEST_CASE("toString and parse round-trip") {
        const Uuid original(0x0123456789abcdefull, 0x8edcba9876543210ull);
        const std::string text = original.toString();
        CHECK(text == "01234567-89ab-cdef-8edc-ba9876543210");

        const std::optional<Uuid> parsed = Uuid::parse(text);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == original);
    }

    TEST_CASE("parse accepts uppercase") {
        const std::optional<Uuid> parsed = Uuid::parse("01234567-89AB-CDEF-8EDC-BA9876543210");
        REQUIRE(parsed.has_value());
        CHECK(parsed->toString() == "01234567-89ab-cdef-8edc-ba9876543210");
    }

    TEST_CASE("parse rejects malformed text") {
        CHECK_FALSE(Uuid::parse("").has_value());
        CHECK_FALSE(Uuid::parse("01234567-89ab-cdef-8edc-ba987654321").has_value());
        CHECK_FALSE(Uuid::parse("01234567-89ab-cdef-8edc-ba98765432100").has_value());
        CHECK_FALSE(Uuid::parse("0123456789abcdef8edcba9876543210").has_value());
        CHECK_FALSE(Uuid::parse("01234567_89ab_cdef_8edc_ba9876543210").has_value());
        CHECK_FALSE(Uuid::parse("0123456g-89ab-cdef-8edc-ba9876543210").has_value());
        CHECK_FALSE(Uuid::parse("01234567-89ab-cdef-8edc-ba987654321-").has_value());
    }

    TEST_CASE("fromName is deterministic and depends on both inputs") {
        const Uuid namespaceA = *Uuid::parse("11111111-1111-4111-8111-111111111111");
        const Uuid namespaceB = *Uuid::parse("22222222-2222-4222-8222-222222222222");

        const Uuid first = Uuid::fromName(namespaceA, "mesh/0");
        CHECK(first == Uuid::fromName(namespaceA, "mesh/0"));
        CHECK(first != Uuid::fromName(namespaceA, "mesh/1"));
        CHECK(first != Uuid::fromName(namespaceB, "mesh/0"));

        CHECK(((first.high() >> 12) & 0xf) == 8);
        CHECK((first.low() >> 62) == 0b10);
    }

    TEST_CASE("fromName is stable across engine versions") {
        // Sub-asset identifiers are stored in project files, so this value must never
        // change. If this fails, existing projects would lose their asset references.
        const Uuid namespaceId = *Uuid::parse("11111111-1111-4111-8111-111111111111");
        CHECK(Uuid::fromName(namespaceId, "mesh/0").toString() == "7c87221f-1dd6-8d48-93da-7d5594886ee3");
    }

    TEST_CASE("Uuids order and hash consistently") {
        const Uuid low(1, 5);
        const Uuid high(2, 0);
        CHECK(low < high);
        CHECK(std::hash<Uuid>()(low) == std::hash<Uuid>()(Uuid(1, 5)));
    }
}
