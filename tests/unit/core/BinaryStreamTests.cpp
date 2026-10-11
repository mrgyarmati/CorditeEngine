#include "cordite/core/BinaryStream.h"

#include "support/TestSupport.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace cordite;

namespace {

struct Vertex {
    float x;
    float y;
    float z;
    uint32_t color;

    bool operator==(const Vertex&) const = default;
};

} // namespace

TEST_SUITE("BinaryStream") {
    TEST_CASE("values, strings and arrays round-trip") {
        const std::vector<Vertex> vertices = {{1.0f, 2.0f, 3.0f, 0xff0000ffu}, {4.0f, 5.0f, 6.0f, 0x00ff00ffu}};

        BinaryWriter writer;
        writer.write<uint32_t>(0x4d455348u);
        writer.write<double>(3.5);
        writer.writeString("barrel_01");
        writer.writeString("");
        writer.writeArray<Vertex>(vertices);
        writer.write<uint8_t>(7);

        BinaryReader reader(writer.data());
        CHECK(reader.read<uint32_t>() == 0x4d455348u);
        CHECK(reader.read<double>() == 3.5);
        CHECK(reader.readString() == "barrel_01");
        CHECK(reader.readString().empty());
        CHECK(reader.readArray<Vertex>() == vertices);
        CHECK(reader.read<uint8_t>() == 7);

        CHECK_FALSE(reader.failed());
        CHECK(reader.remaining() == 0);
        CHECK(reader.status("test").has_value());
    }

    TEST_CASE("the layout is little-endian with explicit length prefixes") {
        BinaryWriter writer;
        writer.write<uint16_t>(0x0102);
        writer.writeString("AB");

        const std::vector<std::byte>& data = writer.data();
        REQUIRE(data.size() == 2 + 4 + 2);
        CHECK(data[0] == std::byte{0x02});
        CHECK(data[1] == std::byte{0x01});
        CHECK(data[2] == std::byte{0x02});
        CHECK(data[3] == std::byte{0x00});
        CHECK(data[6] == std::byte{'A'});
        CHECK(data[7] == std::byte{'B'});
    }

    TEST_CASE("reading past the end fails, returns zero and stays failed") {
        BinaryWriter writer;
        writer.write<uint16_t>(0xbeef);

        BinaryReader reader(writer.data());
        CHECK(reader.read<uint32_t>() == 0);
        CHECK(reader.failed());

        // Even a read that would now fit is refused: the stream position is unreliable.
        CHECK(reader.read<uint8_t>() == 0);
        CHECK(reader.failed());

        const Status status = reader.status("mesh");
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::ParseError);
        CHECK(status.error().message.find("mesh") != std::string::npos);
    }

    TEST_CASE("a corrupt string length does not allocate or read out of bounds") {
        BinaryWriter writer;
        writer.write<uint32_t>(0xffffffffu);
        writer.write<uint8_t>('x');

        BinaryReader reader(writer.data());
        CHECK(reader.readString().empty());
        CHECK(reader.failed());
    }

    TEST_CASE("a corrupt array count does not allocate or read out of bounds") {
        BinaryWriter writer;
        writer.write<uint64_t>(0xffffffffffffffffull);
        writer.write<uint32_t>(1);

        BinaryReader reader(writer.data());
        CHECK(reader.readArray<Vertex>().empty());
        CHECK(reader.failed());
    }

    TEST_CASE("readSpan returns a view without copying") {
        BinaryWriter writer;
        writer.write<uint8_t>(1);
        writer.write<uint8_t>(2);
        writer.write<uint8_t>(3);

        BinaryReader reader(writer.data());
        const std::span<const std::byte> view = reader.readSpan(2);
        REQUIRE(view.size() == 2);
        CHECK(view.data() == writer.data().data());
        CHECK(reader.position() == 2);

        CHECK(reader.readSpan(5).empty());
        CHECK(reader.failed());
    }

    TEST_CASE("take moves the buffer out and resets the writer") {
        BinaryWriter writer;
        writer.write<uint32_t>(1);

        const std::vector<std::byte> taken = writer.take();
        CHECK(taken.size() == 4);
        CHECK(writer.size() == 0);
    }

    TEST_CASE("an empty reader fails cleanly") {
        BinaryReader reader({});
        CHECK(reader.remaining() == 0);
        CHECK(reader.read<uint8_t>() == 0);
        CHECK(reader.failed());
    }
}
