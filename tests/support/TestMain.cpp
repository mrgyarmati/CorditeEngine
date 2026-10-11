#define DOCTEST_CONFIG_IMPLEMENT
#include "support/TestSupport.h"

#include <doctest/doctest.h>

int main(int argc, char** argv) {
    cordite::test::installTestEnvironment();

    doctest::Context context(argc, argv);
    const int result = context.run();

    cordite::Log::shutdown();
    return result;
}
