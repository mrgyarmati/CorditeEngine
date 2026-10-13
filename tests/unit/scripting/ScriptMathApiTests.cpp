#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// Lua that is put before what a test runs: whether two vectors, or two rotations, are
/// the same but for what single floats lose on the way.
constexpr const char* kNear = R"(
    local function near(a, b)
        local worst = 0
        for _, name in ipairs({"x", "y", "z", "w"}) do
            if a[name] ~= nil or b[name] ~= nil then worst = math.max(worst, math.abs(a[name] - b[name])) end
        end
        return worst < 1.0e-5
    end
)";

std::string withNear(const std::string& body) {
    return kNear + body;
}

} // namespace

TEST_SUITE("ScriptMathApi") {
    TEST_CASE("a vector is a table of three numbers that a script reads and changes like any other") {
        ScriptStage stage;

        CHECK(stage.line("print(cordite.vec3(1, 2, 3), cordite.vec3(), cordite.vec3(4), cordite.vec3(0.5, -0.25))") ==
              "(1.0, 2.0, 3.0)\t(0.0, 0.0, 0.0)\t(4.0, 0.0, 0.0)\t(0.5, -0.25, 0.0)");
        CHECK(stage.line("local v = cordite.vec3(1, 2, 3)\n"
                         "v.y = v.y + 10\n"
                         "print(v.x, v.y, v.z, v, type(v), getmetatable(v) == cordite.Vec3)") ==
              "1.0\t12.0\t3.0\t(1.0, 12.0, 3.0)\ttable\ttrue");
        // A copy is a vector of its own.
        CHECK(stage.line("local a = cordite.vec3(1, 2, 3)\n"
                         "local b = a + cordite.vec3()\n"
                         "b.x = 9\n"
                         "print(a, b)") == "(1.0, 2.0, 3.0)\t(9.0, 2.0, 3.0)");
        CHECK(stage.refusal("cordite.vec3('one')") == "bad argument #1 to 'vec3' (number expected, got string)");
    }

    TEST_CASE("vectors add, subtract, scale and compare") {
        ScriptStage stage;
        const std::string a = "local a, b = cordite.vec3(1, 2, 3), cordite.vec3(10, 20, 40)\n";

        CHECK(stage.line(a + "print(a + b, b - a, -a)") == "(11.0, 22.0, 43.0)\t(9.0, 18.0, 37.0)\t(-1.0, -2.0, -3.0)");
        CHECK(
            stage.line(a + "print(a * 2, 2 * a, a * b, b / 2, b / a, 6 / a)") ==
            "(2.0, 4.0, 6.0)\t(2.0, 4.0, 6.0)\t(10.0, 40.0, 120.0)\t(5.0, 10.0, 20.0)\t(10.0, 10.0, 13.333333333333)\t"
            "(6.0, 3.0, 2.0)");
        CHECK(stage.line(a + "print(a == cordite.vec3(1, 2, 3), a == b, a ~= b, a == {x = 1, y = 2, z = 3}, a == 1, "
                             "rawequal(a, cordite.vec3(1, 2, 3)))") == "true\tfalse\ttrue\ttrue\tfalse\tfalse");
        // What a script writes in passing is a vector too: three numbers in a row, or by name.
        CHECK(stage.line(a + "print(a + {1, 1, 1}, a - {x = 1, y = 2, z = 3}, {1, 0, 0} + a)") ==
              "(2.0, 3.0, 4.0)\t(0.0, 0.0, 0.0)\t(2.0, 2.0, 3.0)");
        // Scripts compute as Lua does, in doubles: nothing is lost to a float on the way.
        CHECK(stage.line("local sum = cordite.vec3(0.1, 0.2, 0.3) + cordite.vec3(0.2, 0.1, 1e-12)\n"
                         "print(sum.x == 0.1 + 0.2, sum.z == 0.3 + 1e-12)") == "true\ttrue");
        // Dividing by nothing gives what it gives a number.
        CHECK(stage.line("print(cordite.vec3(1, -1, 0) / 0 == cordite.vec3(math.huge, -math.huge, 0 / 0))") == "false");
        CHECK(stage.line("local v = cordite.vec3(1, -1, 2) / 0 print(v.x, v.y)") == "inf\t-inf");
    }

    TEST_CASE("a vector has a length, a direction and products") {
        ScriptStage stage;

        CHECK(stage.line("local v = cordite.vec3(3, 4, 0)\n"
                         "print(v:length(), v:normalized(), v:normalized():length(), cordite.vec3():normalized())") ==
              "5.0\t(0.6, 0.8, 0.0)\t1.0\t(0.0, 0.0, 0.0)");
        CHECK(stage.line("local x, y = cordite.vec3(1, 0, 0), cordite.vec3(0, 1, 0)\n"
                         "print(x:dot(y), x:dot(x * 3), x:cross(y), y:cross(x), x:distance(y) == math.sqrt(2))") ==
              "0.0\t3.0\t(0.0, 0.0, 1.0)\t(0.0, 0.0, -1.0)\ttrue");
        CHECK(stage.line("local a, b = cordite.vec3(0, 0, 0), cordite.vec3(10, -10, 4)\n"
                         "print(a:lerp(b, 0), a:lerp(b, 0.25), a:lerp(b, 1), a:lerp(b, 2))") ==
              "(0.0, 0.0, 0.0)\t(2.5, -2.5, 1.0)\t(10.0, -10.0, 4.0)\t(20.0, -20.0, 8.0)");
        // The functions take what is shaped like a vector, whoever made it.
        CHECK(stage.line("print(cordite.Vec3.length({3, 4, 0}), cordite.Vec3.dot({x = 1, y = 2, z = 3}, {1, 1, 1}))") ==
              "5.0\t6.0");
    }

    TEST_CASE("what is no vector is refused where one is asked for") {
        ScriptStage stage;

        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + 5") ==
              "bad argument #2 to 'add' (Vec3 expected, got number)");
        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + {1, 2}") ==
              "bad argument #2 to 'add' (Vec3 expected, got table)");
        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + {x = 1, y = 2}") ==
              "bad argument #2 to 'add' (Vec3 expected, got table)");
        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + {x = 1, y = 'two', z = 3}") ==
              "bad argument #2 to 'add' (Vec3 expected, got table)");
        // A rotation is not a vector that has one number more.
        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + cordite.quat()") ==
              "bad argument #2 to 'add' (Vec3 expected, got Quat)");
        CHECK(stage.refusal("local sum = cordite.vec3(1, 2, 3) + {1, 2, 3, 4}") ==
              "bad argument #2 to 'add' (Vec3 expected, got table)");
        CHECK(stage.refusal("cordite.vec3(1, 2, 3):dot()") == "bad argument #1 to 'dot' (Vec3 expected, got no value)");
        CHECK(stage.refusal("cordite.vec3(1, 2, 3):lerp(cordite.vec3(), 'half')") ==
              "bad argument #2 to 'lerp' (number expected, got string)");
    }

    TEST_CASE("a rotation is made of numbers, of angles, of an axis or of a direction to face") {
        ScriptStage stage;

        CHECK(stage.line("print(cordite.quat(), cordite.quat(0, 0, 0, 5), cordite.quat(0, 3, 0, 0), "
                         "getmetatable(cordite.quat()) == cordite.Quat)") ==
              "(0.0, 0.0, 0.0, 1.0)\t(0.0, 0.0, 0.0, 1.0)\t(0.0, 1.0, 0.0, 0.0)\ttrue");
        // One that is written by hand need not have the length one: where a rotation is
        // expected it is brought to it. Twice the rotation that turns nothing turns
        // nothing, and twice a half turn about Y is a half turn.
        CHECK(stage.line("print(cordite.Quat.inverse({0, 0, 0, 2}), cordite.Quat.forward({0, 2, 0, 0}), "
                         "cordite.Quat.forward({x = 0, y = 0, z = 0, w = -4}))") ==
              "(0.0, 0.0, 0.0, 1.0)\t(0.0, 0.0, 1.0)\t(0.0, 0.0, -1.0)");
        // Nothing has no sign: the other way from nowhere is nowhere.
        CHECK(stage.line("print(-cordite.vec3(), cordite.vec3(1, 2, 3) * 0 * -1, cordite.quat():inverse())") ==
              "(0.0, 0.0, 0.0)\t(0.0, 0.0, 0.0)\t(0.0, 0.0, 0.0, 1.0)");
        CHECK(stage.refusal("cordite.quat(0, 0, 0, 0)") == "a rotation has a length, and numbers that are finite");
        CHECK(stage.refusal("cordite.quat(0 / 0, 0, 0, 1)") == "a rotation has a length, and numbers that are finite");

        // Angles in degrees, about X, about Y and about Z; a turn about Y to the left
        // takes forward to the left.
        CHECK(
            stage.line(withNear("local turn = cordite.euler(0, 90, 0)\n"
                                "print(near(turn * cordite.vec3(0, 0, -1), cordite.vec3(-1, 0, 0)),\n"
                                "      near(turn * cordite.vec3(1, 0, 0), cordite.vec3(0, 0, -1)),\n"
                                "      near(cordite.euler(90, 0, 0) * cordite.vec3(0, 0, -1), cordite.vec3(0, 1, 0)),\n"
                                "      near(cordite.euler(0, 0, 90) * cordite.vec3(1, 0, 0), cordite.vec3(0, 1, 0)),\n"
                                "      near(cordite.euler(cordite.vec3(10, 20, 30)), cordite.euler(10, 20, 30)),\n"
                                "      near(cordite.euler(10, 20, 30):euler(), cordite.vec3(10, 20, 30)))")) ==
            "true\ttrue\ttrue\ttrue\ttrue\ttrue");
        CHECK(stage.line(
                  withNear("print(near(cordite.axisAngle(cordite.vec3(0, 1, 0), 90), cordite.euler(0, 90, 0)),\n"
                           "      near(cordite.axisAngle({0, 5, 0}, 90), cordite.euler(0, 90, 0)),\n"
                           "      near(cordite.axisAngle(cordite.vec3(1, 0, 0), -90), cordite.euler(-90, 0, 0)))")) ==
              "true\ttrue\ttrue");
        // Angles are finite, and finite still as the single floats the engine keeps.
        CHECK(stage.refusal("cordite.euler(0, 0 / 0, 0)") ==
              "bad argument #2 to 'euler' (a number that is finite expected)");
        for (const std::string_view tooLarge :
             {"cordite.euler(1e300, 0, 0)", "cordite.euler(0, -1e300, 0)", "cordite.euler(0, 0, 1e300)"}) {
            CAPTURE(std::string(tooLarge));
            CHECK(stage.refusal(std::string(tooLarge)) == "three angles that are finite, and not too large, expected");
        }
        CHECK(stage.refusal("cordite.euler(10, 20)") == "bad argument #3 to 'euler' (number expected, got no value)");
        CHECK(stage.refusal("cordite.axisAngle(cordite.vec3(), 90)") ==
              "bad argument #1 to 'axisAngle' (an axis has a direction: a vector of some length)");
        CHECK(stage.refusal("cordite.axisAngle(cordite.vec3(0, 1, 0), 1 / 0)") ==
              "bad argument #2 to 'axisAngle' (a number that is finite expected)");

        // Forward is -Z; a rotation that looks somewhere has its forward there.
        CHECK(stage.line(withNear(
                  "local east = cordite.lookRotation(cordite.vec3(5, 0, 0))\n"
                  "local down = cordite.lookRotation(cordite.vec3(0, -1, 0), cordite.vec3(0, 0, -1))\n"
                  "print(near(east:forward(), cordite.vec3(1, 0, 0)), near(east:up(), cordite.vec3(0, 1, 0)),\n"
                  "      near(east:right(), cordite.vec3(0, 0, 1)), near(down:forward(), cordite.vec3(0, -1, 0)),\n"
                  "      near(down:up(), cordite.vec3(0, 0, -1)))")) == "true\ttrue\ttrue\ttrue\ttrue");
        CHECK(stage.refusal("cordite.lookRotation(cordite.vec3())") ==
              "bad argument #1 to 'lookRotation' (a direction to look in: a vector of some length)");
    }

    TEST_CASE("rotations follow one another, turn vectors and are undone") {
        ScriptStage stage;

        // The rotation that turns nothing has its axes where the world has them.
        CHECK(stage.line(
                  "local still = cordite.quat()\n"
                  "print(still:forward(), still:right(), still:up(), still * cordite.vec3(1, 2, 3), still:euler())") ==
              "(0.0, 0.0, -1.0)\t(1.0, 0.0, 0.0)\t(0.0, 1.0, 0.0)\t(1.0, 2.0, 3.0)\t(0.0, 0.0, 0.0)");
        // q * r is r first, then q.
        CHECK(stage.line(withNear("local yaw, pitch = cordite.euler(0, 90, 0), cordite.euler(90, 0, 0)\n"
                                  "local forward = cordite.vec3(0, 0, -1)\n"
                                  "print(near((yaw * pitch) * forward, yaw * (pitch * forward)),\n"
                                  "      near((yaw * pitch) * forward, cordite.vec3(0, 1, 0)),\n"
                                  "      near((pitch * yaw) * forward, cordite.vec3(-1, 0, 0)),\n"
                                  "      near(yaw * yaw:inverse(), cordite.quat()),\n"
                                  "      near(yaw:inverse() * (yaw * forward), forward))")) ==
              "true\ttrue\ttrue\ttrue\ttrue");
        CHECK(stage.line(withNear("local from, to = cordite.quat(), cordite.euler(0, 90, 0)\n"
                                  "print(near(from:slerp(to, 0), from), near(from:slerp(to, 1), to),\n"
                                  "      near(from:slerp(to, 0.5), cordite.euler(0, 45, 0)),\n"
                                  "      near(from:slerp(to, 1 / 3):euler(), cordite.vec3(0, 30, 0)))")) ==
              "true\ttrue\ttrue\ttrue");
        CHECK(stage.line("local q = cordite.euler(10, 20, 30)\n"
                         "print(q == cordite.euler(10, 20, 30), q == cordite.quat(), q ~= cordite.quat(), "
                         "cordite.quat() == {x = 0, y = 0, z = 0, w = 1})") == "true\tfalse\ttrue\ttrue");

        CHECK(stage.refusal("local turned = cordite.quat() * 5") ==
              "bad argument #2 to 'mul' (Vec3 expected, got number)");
        CHECK(stage.refusal("cordite.quat():slerp(cordite.vec3(), 0.5)") ==
              "bad argument #1 to 'slerp' (Quat expected, got Vec3)");
        CHECK(stage.refusal("cordite.Quat.inverse({0, 0, 0, 0})") ==
              "bad argument #1 to 'inverse' (a rotation has a length, and numbers that are finite)");
    }

    TEST_CASE("the functions of vectors and rotations are said in words") {
        ScriptStage stage;
        size_t counted = 0;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            const bool isMath = function.name.starts_with("cordite.Vec3.") ||
                                function.name.starts_with("cordite.Quat.") || function.name == "cordite.vec3" ||
                                function.name == "cordite.quat" || function.name == "cordite.euler" ||
                                function.name == "cordite.axisAngle" || function.name == "cordite.lookRotation";
            if (!isMath) {
                continue;
            }
            ++counted;
            CAPTURE(function.name);
            CHECK(function.signature.find("->") != std::string::npos);
            CHECK(function.description.size() > 20);
            CHECK(function.description.back() == '.');
        }
        CHECK(counted == 5 + 13 + 9);
    }
}
