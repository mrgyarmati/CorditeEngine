#include "cordite/core/JobSystem.h"

#include "cordite/core/Log.h"

#include "support/TestSupport.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace cordite;

TEST_SUITE("JobSystem") {
    TEST_CASE("the thread count honours the request") {
        JobSystem single(1);
        CHECK(single.threadCount() == 1);

        JobSystem four(4);
        CHECK(four.threadCount() == 4);

        JobSystem automatic;
        CHECK(automatic.threadCount() >= 1);
    }

    TEST_CASE("parallelFor visits every index exactly once") {
        JobSystem jobs(4);

        constexpr uint32_t kCount = 100000;
        std::vector<std::atomic<uint8_t>> visits(kCount);
        jobs.parallelFor(kCount, 128, [&visits](uint32_t begin, uint32_t end) {
            for (uint32_t index = begin; index < end; ++index) {
                ++visits[index];
            }
        });

        bool allOnce = true;
        for (const std::atomic<uint8_t>& count : visits) {
            allOnce = allOnce && count.load() == 1;
        }
        CHECK(allOnce);
    }

    TEST_CASE("parallelFor groups items into batches") {
        JobSystem jobs(4);

        constexpr uint32_t kCount = 100000;
        constexpr uint32_t kMinBatch = 1000;
        std::atomic<uint32_t> batches{0};
        std::atomic<uint32_t> total{0};
        jobs.parallelFor(kCount, kMinBatch, [&](uint32_t begin, uint32_t end) {
            total += end - begin;
            ++batches;
        });

        CHECK(total.load() == kCount);
        // The batch size is a hint, so allow a few short remainders per thread, but the
        // body must not be invoked anywhere near once per item.
        CHECK(batches.load() <= kCount / kMinBatch + 4 * jobs.threadCount());
    }

    TEST_CASE("parallelFor with zero items does not invoke the body") {
        JobSystem jobs(2);
        bool called = false;
        jobs.parallelFor(0, 1, [&called](uint32_t, uint32_t) { called = true; });
        CHECK_FALSE(called);
    }

    TEST_CASE("parallelFor works on a single-threaded pool") {
        JobSystem jobs(1);
        uint64_t sum = 0;
        jobs.parallelFor(1000, 10, [&sum](uint32_t begin, uint32_t end) {
            for (uint32_t index = begin; index < end; ++index) {
                sum += index;
            }
        });
        CHECK(sum == 499500);
    }

    TEST_CASE("submitted jobs run and can be waited on individually") {
        JobSystem jobs(4);

        std::atomic<int> value{0};
        const JobHandle first = jobs.submit([&value] { value += 1; });
        const JobHandle second = jobs.submit([&value] { value += 10; });
        CHECK(first.valid());

        jobs.wait(first);
        jobs.wait(second);
        CHECK(value.load() == 11);
    }

    TEST_CASE("waitAll blocks until every job has finished") {
        JobSystem jobs(4);

        std::atomic<int> completed{0};
        for (int index = 0; index < 500; ++index) {
            jobs.submit([&completed] { ++completed; });
        }
        jobs.waitAll();
        CHECK(completed.load() == 500);
    }

    TEST_CASE("jobs whose handles were dropped still complete before destruction") {
        std::atomic<int> completed{0};
        {
            JobSystem jobs(4);
            for (int index = 0; index < 200; ++index) {
                jobs.submit([&completed] { ++completed; });
            }
        }
        CHECK(completed.load() == 200);
    }

    TEST_CASE("waiting on an empty handle returns immediately") {
        JobSystem jobs(2);
        const JobHandle empty;
        CHECK_FALSE(empty.valid());
        CHECK_NOTHROW(jobs.wait(empty));
    }

    TEST_CASE("an exception in a job is rethrown from wait") {
        JobSystem jobs(4);
        const JobHandle handle = jobs.submit([] { throw std::runtime_error("job failed"); });
        CHECK_THROWS_WITH_AS(jobs.wait(handle), "job failed", std::runtime_error);

        // The exception is delivered once; the pool stays usable.
        CHECK_NOTHROW(jobs.wait(handle));
        std::atomic<bool> ran{false};
        jobs.wait(jobs.submit([&ran] { ran = true; }));
        CHECK(ran.load());
    }

    TEST_CASE("an exception in parallelFor is rethrown to the caller") {
        JobSystem jobs(4);
        CHECK_THROWS_AS(jobs.parallelFor(1000, 10,
                                         [](uint32_t begin, uint32_t) {
                                             if (begin == 0) {
                                                 throw std::logic_error("range failed");
                                             }
                                         }),
                        std::logic_error);
    }

    TEST_CASE("waitAll rethrows a failure from a job whose handle was dropped") {
        JobSystem jobs(4);
        jobs.submit([] { throw std::runtime_error("dropped handle"); });
        CHECK_THROWS_WITH_AS(jobs.waitAll(), "dropped handle", std::runtime_error);
        CHECK_NOTHROW(jobs.waitAll());
    }

    TEST_CASE("a failure nobody waits for is logged, not lost") {
        cordite::test::LogCapture capture(LogLevel::Error);
        {
            JobSystem jobs(2);
            jobs.submit([] { throw std::runtime_error("nobody listened"); });
        }
        CHECK(capture.count(LogLevel::Error) == 1);
        CHECK(capture.contains("nobody listened"));
    }

    TEST_CASE("use from a foreign thread is caught, or degrades to running inline") {
        JobSystem jobs(2);
        std::atomic<bool> ran{false};
        std::atomic<bool> asserted{false};

        std::thread foreign([&] {
            try {
                const JobHandle handle = jobs.submit([&ran] { ran = true; });
                jobs.wait(handle);
                jobs.parallelFor(10, 1, [](uint32_t, uint32_t) {});
            } catch (const cordite::test::AssertionFailure&) {
                asserted = true;
            }
        });
        foreign.join();

#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK(asserted.load());
        CHECK_FALSE(ran.load());
#else
        CHECK_FALSE(asserted.load());
        CHECK(ran.load());
#endif
        // The pool is unharmed either way.
        std::atomic<bool> stillWorks{false};
        jobs.wait(jobs.submit([&stillWorks] { stillWorks = true; }));
        CHECK(stillWorks.load());
    }

    TEST_CASE("jobs may submit further jobs") {
        JobSystem jobs(4);
        std::atomic<int> value{0};
        const JobHandle outer = jobs.submit([&jobs, &value] {
            const JobHandle inner = jobs.submit([&value] { value += 5; });
            jobs.wait(inner);
            value += 1;
        });
        jobs.wait(outer);
        CHECK(value.load() == 6);
    }
}
