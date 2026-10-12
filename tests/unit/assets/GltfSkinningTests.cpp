#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/GltfImporter.h"
#include "cordite/core/Json.h"

#include "AssetTestSupport.h"
#include "GltfTestSupport.h"
#include "support/SkinnedModelSupport.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::GltfFile;
using cordite::test::kGltfFloat;
using cordite::test::kGltfUnsignedByte;
using cordite::test::kGltfUnsignedShort;
using cordite::test::makeSkinnedBar;
using cordite::test::parseOrFail;
using cordite::test::skinnedCorners;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// True when one of the warnings of a document says `text`.
bool warns(const GltfDocument& document, std::string_view text) {
    return std::ranges::any_of(document.warnings,
                               [&](const std::string& warning) { return warning.find(text) != std::string::npos; });
}

/// A file with one triangle on two joints, for a test to change before it is written:
/// node 0 has the mesh and the skin, node 1 ("Hip") and its child node 2 ("Knee") are
/// the joints. Accessors: 0 positions, 1 joints, 2 weights, 3 inverse bind matrices.
GltfFile triangleOnTwoJoints() {
    GltfFile file;
    file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 0, 1, 0, 0, 0, 1, 0});
    file.addAccessor<uint8_t>("VEC4", kGltfUnsignedByte, {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0});
    file.addAccessor<float>("VEC4", kGltfFloat, {1, 0, 0, 0, 1, 0, 0, 0, 0.25f, 0.75f, 0, 0});
    file.addAccessor<float>("MAT4", kGltfFloat, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,  0, 1,
                                                 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -2, 0, 1});
    file.json["meshes"] = parseOrFail(
        R"([{"name": "Leg", "primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}}]}])");
    file.json["nodes"] = parseOrFail(R"([{"name": "Body", "mesh": 0, "skin": 0},
                                         {"name": "Hip", "children": [2], "translation": [0, 1, 0]},
                                         {"name": "Knee", "translation": [0, 2, 0]}])");
    file.json["skins"] = parseOrFail(R"([{"name": "Bones", "joints": [1, 2], "inverseBindMatrices": 3}])");
    file.json["scenes"] = parseOrFail(R"([{"nodes": [0, 1]}])");
    file.json["scene"] = 0;
    return file;
}

/// Adds an animation with one channel to a file: `times`, and `values` of `type` for
/// `path` of `node`.
void addChannel(GltfFile& file, int node, std::string_view path, std::string_view type, const std::vector<float>& times,
                const std::vector<float>& values, std::string_view interpolation = "LINEAR") {
    const int input = file.addAccessor<float>("SCALAR", kGltfFloat, times);
    const int output = file.addAccessor<float>(type, kGltfFloat, values);
    if (!file.json.contains("animations")) {
        file.json["animations"] = parseOrFail(R"([{"name": "Walk", "samplers": [], "channels": []}])");
    }
    Json& animation = file.json["animations"][0];
    Json sampler = Json::object();
    sampler["input"] = input;
    sampler["output"] = output;
    sampler["interpolation"] = interpolation;
    Json channel = Json::object();
    channel["sampler"] = animation["samplers"].size();
    channel["target"]["node"] = node;
    channel["target"]["path"] = path;
    animation["samplers"].push_back(std::move(sampler));
    animation["channels"].push_back(std::move(channel));
}

} // namespace

TEST_SUITE("GltfSkinning") {
    TEST_CASE("a skinned model with its animations is written and read back the same") {
        const TempDirectory directory;
        const GltfDocument document = makeSkinnedBar();
        for (const char* name : {"bar.gltf", "bar.glb"}) {
            CAPTURE(std::string(name));
            REQUIRE(exportGltf(document, directory.path() / name).has_value());
            const GltfDocument imported = unwrap(importGltf(directory.path() / name));
            CHECK(imported.warnings.empty());
            CHECK(imported.roots == document.roots);
            CHECK(imported.nodes == document.nodes);
            CHECK(imported.skins == document.skins);
            CHECK(imported.animations == document.animations);

            REQUIRE(imported.meshes.size() == 1);
            const MeshData& mesh = imported.meshes[0].mesh;
            CHECK(mesh.isSkinned());
            CHECK(mesh.vertices.size() == document.meshes[0].mesh.vertices.size());
            CHECK(mesh.skin.size() == mesh.vertices.size());
            CHECK(skinnedCorners(mesh) == skinnedCorners(document.meshes[0].mesh));
            CHECK(validateMesh(mesh).has_value());
        }

        // What was read is what the builder made: a skeleton of three joints, one above
        // the other, bound where they stand.
        const SkeletonData& skeleton = document.skins[0].skeleton;
        REQUIRE(skeleton.joints.size() == 3);
        CHECK(skeleton.name == "Bar skin");
        CHECK(skeleton.joints[0].name == "Root");
        CHECK(skeleton.joints[0].parent == kNoJoint);
        CHECK(skeleton.joints[1].name == "Middle");
        CHECK(skeleton.joints[1].parent == 0);
        CHECK(skeleton.joints[2].parent == 1);
        CHECK(skeleton.joints[2].rest.position == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(skeleton.joints[2].inverseBind[3] == Vec4(0.0f, -2.0f, 0.0f, 1.0f));
        CHECK(skeleton.joints[2].parentOffset == Mat4(1.0f));
    }

    TEST_CASE("joints, weights and inverse bind matrices are read from the forms a file may have them in") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        GltfDocument document = unwrap(importGltf(file.write(directory.path(), "bytes")));
        CHECK(document.warnings.empty());
        REQUIRE(document.meshes.size() == 1);
        const MeshData& mesh = document.meshes[0].mesh;
        REQUIRE(mesh.skin.size() == 3);
        CHECK(jointsUsed(mesh) == 2);
        // One corner follows both joints, a quarter and three quarters.
        const auto corners = skinnedCorners(mesh);
        CHECK(corners[0] == std::array<float, 11>{0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0});
        CHECK(corners[1] == std::array<float, 11>{0, 1, 0, 1, 0.75f, 0, 0.25f, 0, 0, 0, 0});
        CHECK(corners[2] == std::array<float, 11>{1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0});

        REQUIRE(document.skins.size() == 1);
        const GltfSkin& skin = document.skins[0];
        CHECK(skin.joints == std::vector<uint32_t>{1, 2});
        CHECK(skin.skeleton.name == "Bones");
        REQUIRE(skin.skeleton.joints.size() == 2);
        CHECK(skin.skeleton.joints[0].name == "Hip");
        CHECK(skin.skeleton.joints[0].parent == kNoJoint);
        CHECK(skin.skeleton.joints[0].rest.position == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(skin.skeleton.joints[0].inverseBind == Mat4(1.0f));
        CHECK(skin.skeleton.joints[1].name == "Knee");
        CHECK(skin.skeleton.joints[1].parent == 0);
        CHECK(skin.skeleton.joints[1].inverseBind[3] == Vec4(0.0f, -2.0f, 0.0f, 1.0f));
        CHECK(document.nodes[0].skin == 0u);
        CHECK_FALSE(document.nodes[1].skin.has_value());

        // Joints as shorts, weights as shorts that stand for zero to one.
        file = triangleOnTwoJoints();
        file.accessors()[1] = file.accessors()[file.addAccessor<uint16_t>("VEC4", kGltfUnsignedShort,
                                                                          {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0})];
        file.accessors()[2] = file.accessors()[file.addAccessor<uint16_t>(
            "VEC4", kGltfUnsignedShort, {65535, 0, 0, 0, 65535, 0, 0, 0, 16384, 49151, 0, 0}, true)];
        document = unwrap(importGltf(file.write(directory.path(), "shorts")));
        CHECK(document.warnings.empty());
        const auto shortCorners = skinnedCorners(document.meshes[0].mesh);
        CHECK(shortCorners[0] == corners[0]);
        CHECK(shortCorners[2] == corners[2]);
        CHECK(shortCorners[1][4] == doctest::Approx(0.75f).epsilon(0.001));
        CHECK(shortCorners[1][6] == doctest::Approx(0.25f).epsilon(0.001));

        // Without inverse bind matrices the joints are bound where they are.
        file = triangleOnTwoJoints();
        file.json["skins"][0].erase("inverseBindMatrices");
        document = unwrap(importGltf(file.write(directory.path(), "unbound")));
        CHECK(document.warnings.empty());
        CHECK(document.skins[0].skeleton.joints[1].inverseBind == Mat4(1.0f));
        CHECK(document.nodes[0].skin == 0u);
    }

    TEST_CASE("weights are brought to one in all, and vertices without any are reported") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        file.accessors()[2] =
            file.accessors()[file.addAccessor<float>("VEC4", kGltfFloat, {2, 2, 0, 0, 0, 0, 0, 0, 0.25f, 0.25f, 0, 0})];
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "weights")));
        const MeshData& mesh = document.meshes[0].mesh;
        REQUIRE(validateMesh(mesh).has_value());
        const auto corners = skinnedCorners(mesh);
        // The first corner follows joint 0 twice, half and half: all of it, as far as a
        // skeleton is concerned.
        CHECK(corners[0][4] + corners[0][6] == doctest::Approx(1.0f));
        // The corner without weights follows its first joint, which is joint 1.
        CHECK(corners[2] == std::array<float, 11>{1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0});
        CHECK(corners[1][4] == 0.5f);
        CHECK(corners[1][6] == 0.5f);
        CHECK(warns(document, "mesh 'Leg' has vertices without weights"));
    }

    TEST_CASE("what a file says of joints that makes no sense is left out, and said") {
        const TempDirectory directory;

        // Joints without weights say nothing: the mesh is rigid, and its node has no skin.
        GltfFile file = triangleOnTwoJoints();
        file.json["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
        GltfDocument document = unwrap(importGltf(file.write(directory.path(), "weightless")));
        CHECK_FALSE(document.meshes[0].mesh.isSkinned());
        CHECK_FALSE(document.nodes[0].skin.has_value());
        CHECK(warns(document, "joints without weights"));
        CHECK(warns(document, "node 'Body' gives the mesh 'Leg' a skin, but its vertices follow no joints"));

        // A skin that lists a node twice is no skeleton; it stays in its place, empty.
        file = triangleOnTwoJoints();
        file.json["skins"][0]["joints"] = parseOrFail("[1, 1]");
        document = unwrap(importGltf(file.write(directory.path(), "twice")));
        REQUIRE(document.skins.size() == 1);
        CHECK(document.skins[0].joints.empty());
        CHECK(document.skins[0].skeleton.joints.empty());
        CHECK(document.skins[0].skeleton.name == "Bones");
        CHECK_FALSE(document.nodes[0].skin.has_value());
        CHECK(warns(document, "lists the node 'Hip' as a joint twice"));
        // The mesh is still there, and still says what it follows.
        CHECK(document.meshes[0].mesh.isSkinned());

        // A skin with fewer joints than the mesh follows cannot move it.
        file = triangleOnTwoJoints();
        file.json["skins"][0]["joints"] = parseOrFail("[2]");
        file.json["skins"][0].erase("inverseBindMatrices");
        document = unwrap(importGltf(file.write(directory.path(), "short")));
        CHECK(document.skins[0].joints == std::vector<uint32_t>{2});
        CHECK_FALSE(document.nodes[0].skin.has_value());
        CHECK(warns(document, "the mesh 'Leg' follows joint 1, but the skin 'Bones' has only 1"));

        // Matrices that hold no numbers: those joints are bound where they are.
        file = triangleOnTwoJoints();
        std::vector<float> matrices(32, 0.0f);
        matrices[0] = matrices[5] = matrices[10] = matrices[15] = 1.0f;
        matrices[16] = kNotANumber;
        file.accessors()[3] = file.accessors()[file.addAccessor<float>("MAT4", kGltfFloat, matrices)];
        document = unwrap(importGltf(file.write(directory.path(), "nan")));
        CHECK(document.skins[0].skeleton.joints[1].inverseBind == Mat4(1.0f));
        CHECK(document.nodes[0].skin == 0u);
        CHECK(warns(document, "hold numbers that are none"));

        // More than four joints a vertex: the first four are kept.
        file = triangleOnTwoJoints();
        file.json["meshes"][0]["primitives"][0]["attributes"]["JOINTS_1"] = 1;
        file.json["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_1"] = 2;
        document = unwrap(importGltf(file.write(directory.path(), "eight")));
        CHECK(document.meshes[0].mesh.isSkinned());
        CHECK(warns(document, "more than four joints"));
    }

    TEST_CASE("a file whose joints cannot be read is refused") {
        const TempDirectory directory;
        const auto refused = [&](GltfFile& file, std::string_view name, std::string_view because) {
            const Result<GltfDocument> result = importGltf(file.write(directory.path(), name));
            REQUIRE_FALSE(result.has_value());
            const std::string& message = result.error().message;
            CHECK_MESSAGE(message.find(because) != std::string::npos, message);
        };

        // Joints are whole numbers.
        GltfFile file = triangleOnTwoJoints();
        file.accessors()[1] = file.accessors()[2];
        refused(file, "floats", "JOINTS_0 of part 0 of mesh 'Leg' is not four unsigned bytes or shorts");

        // One set of joints and of weights for every position: that much is asked of
        // every attribute of a mesh, and checked before anything is read.
        file = triangleOnTwoJoints();
        file.accessors()[1] =
            file.accessors()[file.addAccessor<uint8_t>("VEC4", kGltfUnsignedByte, {0, 0, 0, 0, 1, 0, 0, 0})];
        refused(file, "fewer", "does not follow the glTF 2.0 specification");

        // Likewise a value for every key of an animation, ...
        file = triangleOnTwoJoints();
        addChannel(file, 1, "translation", "VEC3", {0.0f, 1.0f, 2.0f}, {0, 0, 0, 1, 1, 1});
        refused(file, "values", "does not follow the glTF 2.0 specification");
        // ... three of them for a spline, ...
        file = triangleOnTwoJoints();
        addChannel(file, 1, "translation", "VEC3", {0.0f, 1.0f}, {0, 0, 0, 1, 1, 1}, "CUBICSPLINE");
        refused(file, "spline", "does not follow the glTF 2.0 specification");
        // ... and morph targets to move, for an animation that moves them.
        file = triangleOnTwoJoints();
        addChannel(file, 0, "weights", "SCALAR", {0.0f, 1.0f}, {0, 1});
        refused(file, "targets", "does not follow the glTF 2.0 specification");

        // A vertex cannot follow a joint no skeleton may have.
        file = triangleOnTwoJoints();
        file.accessors()[1] = file.accessors()[file.addAccessor<uint16_t>("VEC4", kGltfUnsignedShort,
                                                                          {0, 0, 0, 0, 1, 0, 0, 0, 0, 5000, 0, 0})];
        refused(file, "far", "names joint 5000");

        // A matrix for every joint.
        file = triangleOnTwoJoints();
        file.accessors()[3] = file.accessors()[file.addAccessor<float>(
            "MAT4", kGltfFloat, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})];
        refused(file, "one-matrix", "are not a matrix for each of its 2 joints");

        // Data that is not in the file.
        file = triangleOnTwoJoints();
        file.views()[3]["byteLength"] = 32;
        refused(file, "truncated", "");
    }

    TEST_CASE("joints hang on the nearest joint above them, and what is in between is kept as their offset") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        // An armature above the joints, placed and scaled, and a plain node between them.
        file.json["nodes"] = parseOrFail(R"([
            {"name": "Body", "mesh": 0, "skin": 0},
            {"name": "Armature", "children": [2], "translation": [10, 0, 0], "scale": [2, 2, 2]},
            {"name": "Hip", "children": [3], "translation": [0, 1, 0]},
            {"name": "Spacer", "children": [4], "translation": [0, 0, 5]},
            {"name": "Knee", "translation": [0, 2, 0]}])");
        file.json["skins"][0]["joints"] = parseOrFail("[4, 2]");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "armature")));
        CHECK(document.warnings.empty());
        const SkeletonData& skeleton = document.skins[0].skeleton;
        REQUIRE(skeleton.joints.size() == 2);
        // In the order of the file: the knee first, though it hangs on the hip.
        CHECK(skeleton.joints[0].name == "Knee");
        CHECK(skeleton.joints[0].parent == 1);
        CHECK(skeleton.joints[0].rest.position == Vec3(0.0f, 2.0f, 0.0f));
        CHECK(skeleton.joints[0].parentOffset[3] == Vec4(0.0f, 0.0f, 5.0f, 1.0f));
        CHECK(skeleton.joints[1].name == "Hip");
        CHECK(skeleton.joints[1].parent == kNoJoint);
        CHECK(skeleton.joints[1].parentOffset[3] == Vec4(10.0f, 0.0f, 0.0f, 1.0f));
        CHECK(skeleton.joints[1].parentOffset[0] == Vec4(2.0f, 0.0f, 0.0f, 0.0f));
        CHECK(jointOrder(skeleton) == std::vector<uint32_t>{1, 0});
    }

    TEST_CASE("nodes of the same name are told apart as joints, and in the tracks that move them") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        file.json["nodes"][1]["name"] = "Bone";
        file.json["nodes"][2]["name"] = "Bone";
        addChannel(file, 2, "translation", "VEC3", {0.0f, 1.0f}, {0, 0, 0, 1, 2, 3});
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "twins")));
        CHECK(document.warnings.empty());
        CHECK(document.skins[0].skeleton.joints[0].name == "Bone #1");
        CHECK(document.skins[0].skeleton.joints[1].name == "Bone #2");
        REQUIRE(document.animations.size() == 1);
        CHECK(document.animations[0].animation.tracks[0].joint == "Bone #2");
        CHECK(jointNameOf(document, 0) == "Body");
        CHECK(jointNameOf(document, 2) == "Bone #2");
        CHECK(jointNameOf(document, 99).empty());
    }

    TEST_CASE("an animation is read as curves: stepped, straight, and as splines with their slopes") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        addChannel(file, 2, "translation", "VEC3", {0.0f, 0.5f, 2.0f}, {0, 0, 0, 1, 2, 3, 4, 5, 6});
        addChannel(file, 2, "scale", "VEC3", {0.25f, 1.0f}, {1, 1, 1, 2, 2, 2}, "STEP");
        // A spline: for each key a slope in, a value, a slope out.
        addChannel(file, 1, "translation", "VEC3", {0.0f, 1.0f}, {9, 9, 9, 0, 0, 0, 1, 0, 0, 0, 2, 0, 5, 0, 0, 7, 7, 7},
                   "CUBICSPLINE");
        // A quarter turn about +Y, written the long way round in its second key.
        addChannel(file, 1, "rotation", "VEC4", {0.0f, 1.0f, 3.0f},
                   {0, 0, 0, 1, 0, -0.70710678f, 0, -0.70710678f, 0, 2, 0, 0});

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "curves")));
        CHECK(document.warnings.empty());
        REQUIRE(document.animations.size() == 1);
        const GltfAnimation& walk = document.animations[0];
        CHECK(walk.animation.name == "Walk");
        CHECK(walk.animation.duration == 3.0f);
        CHECK(walk.nodes == std::vector<uint32_t>{2, 1});
        REQUIRE(walk.animation.tracks.size() == 2);
        CHECK(validateAnimation(walk.animation).has_value());

        const AnimationTrack& knee = walk.animation.tracks[0];
        CHECK(knee.joint == "Knee");
        REQUIRE(knee.translation.keys().size() == 3);
        CHECK(knee.translation.keys()[1].time == 0.5f);
        CHECK(knee.translation.keys()[1].value == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(knee.translation.keys()[1].interpolation == Interpolation::Linear);
        CHECK(knee.translation.evaluate(1.25f) == Vec3(2.5f, 3.5f, 4.5f));
        REQUIRE(knee.scale.keys().size() == 2);
        CHECK(knee.scale.keys()[0].interpolation == Interpolation::Step);
        CHECK(knee.scale.evaluate(0.9f) == Vec3(1.0f));
        CHECK(knee.rotation.empty());

        const AnimationTrack& hip = walk.animation.tracks[1];
        CHECK(hip.joint == "Hip");
        REQUIRE(hip.translation.keys().size() == 2);
        CHECK(hip.translation.keys()[0].interpolation == Interpolation::Cubic);
        CHECK(hip.translation.keys()[0].inTangent == Vec3(9.0f));
        CHECK(hip.translation.keys()[0].value == Vec3(0.0f));
        CHECK(hip.translation.keys()[0].outTangent == Vec3(1.0f, 0.0f, 0.0f));
        CHECK(hip.translation.keys()[1].inTangent == Vec3(0.0f, 2.0f, 0.0f));
        CHECK(hip.translation.keys()[1].value == Vec3(5.0f, 0.0f, 0.0f));
        CHECK(hip.translation.keys()[1].outTangent == Vec3(7.0f));
        // Half way: the value of the spline through both keys with those slopes.
        CHECK_NEAR(hip.translation.evaluate(0.5f), Vec3(2.625f, -0.25f, 0.0f), 1.0e-6f);

        // Rotations come out of unit length, each the same way round as the one before.
        REQUIRE(hip.rotation.keys().size() == 3);
        CHECK(hip.rotation.keys()[0].value == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        CHECK_NEAR(hip.rotation.keys()[1].value, Vec4(0.0f, 0.70710678f, 0.0f, 0.70710678f), 1.0e-6f);
        CHECK(hip.rotation.keys()[2].value == Vec4(0.0f, 1.0f, 0.0f, 0.0f));
    }

    TEST_CASE("channels that make no sense are left out, one by one, and said") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        addChannel(file, 2, "translation", "VEC3", {0.0f, 1.0f}, {0, 0, 0, 1, 1, 1});
        // Times that run backwards, or stand still.
        addChannel(file, 2, "scale", "VEC3", {1.0f, 0.5f}, {1, 1, 1, 2, 2, 2});
        addChannel(file, 1, "scale", "VEC3", {0.5f, 0.5f}, {1, 1, 1, 2, 2, 2});
        // Values of another kind than what they move.
        addChannel(file, 1, "translation", "VEC4", {0.0f, 1.0f}, {0, 0, 0, 1, 1, 1, 1, 1});
        // A value that is no number, a time before the beginning.
        addChannel(file, 2, "rotation", "VEC4", {0.0f, 1.0f}, {0, 0, 0, 1, 0, kNotANumber, 0, 1});
        addChannel(file, 1, "rotation", "VEC4", {-1.0f, 1.0f}, {0, 0, 0, 1, 0, 0, 0, 1});
        // The same thing twice: the first is kept.
        addChannel(file, 2, "translation", "VEC3", {0.0f, 4.0f}, {7, 7, 7, 8, 8, 8});
        // Morph targets are not imported, nor what moves them.
        file.json["meshes"][0]["primitives"][0]["targets"] = parseOrFail(R"([{"POSITION": 0}])");
        addChannel(file, 0, "weights", "SCALAR", {0.0f, 1.0f}, {0, 1});

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "channels")));
        REQUIRE(document.animations.size() == 1);
        const AnimationData& walk = document.animations[0].animation;
        REQUIRE(walk.tracks.size() == 2);
        CHECK(walk.tracks[0].joint == "Knee");
        CHECK(walk.tracks[0].translation.keys().size() == 2);
        CHECK(walk.tracks[0].translation.keys()[1].value == Vec3(1.0f));
        CHECK(walk.tracks[0].scale.empty());
        CHECK(walk.tracks[0].rotation.empty());
        CHECK(walk.tracks[1].joint == "Hip");
        CHECK(walk.tracks[1].translation.empty());
        CHECK(walk.tracks[1].scale.empty());
        CHECK(walk.tracks[1].rotation.empty());
        CHECK(walk.duration == 1.0f);
        CHECK(validateAnimation(walk).has_value());

        CHECK(
            warns(document, "the scale of 'Knee' in animation 'Walk' has times that are none, or do not run forward"));
        CHECK(warns(document, "the scale of 'Hip' in animation 'Walk' has times"));
        CHECK(warns(document, "the translation of 'Hip' in animation 'Walk' has no keys, or not a value for each"));
        CHECK(warns(document, "the rotation of 'Knee' in animation 'Walk' has values that are no numbers"));
        CHECK(warns(document, "the rotation of 'Hip' in animation 'Walk' has times"));
        CHECK(warns(document, "the translation of 'Knee' in animation 'Walk' is given more than once"));
        CHECK(warns(document, "animation 'Walk' moves morph targets"));
    }

    TEST_CASE("a file may hold only so many keys") {
        const TempDirectory directory;
        GltfFile file = triangleOnTwoJoints();
        addChannel(file, 2, "translation", "VEC3", {0.0f, 1.0f}, {0, 0, 0, 1, 1, 1});
        addChannel(file, 1, "translation", "VEC3", {0.0f, 1.0f}, {0, 0, 0, 1, 1, 1});
        const std::filesystem::path path = file.write(directory.path(), "keys");
        CHECK(importGltf(path, {.maxAnimationKeys = 4}).has_value());
        const Result<GltfDocument> result = importGltf(path, {.maxAnimationKeys = 3});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::OutOfRange);
        CHECK(result.error().message.find("more keys than can be imported") != std::string::npos);
        // Without its data a file's keys are not read, and not counted.
        CHECK(importGltf(path, {.loadData = false, .maxAnimationKeys = 0}).has_value());
    }

    TEST_CASE("the structure of a file alone says which skins and animations it has") {
        const TempDirectory directory;
        REQUIRE(exportGltf(makeSkinnedBar(), directory.path() / "bar.gltf").has_value());
        const GltfDocument document = unwrap(importGltf(directory.path() / "bar.gltf", {.loadData = false}));
        REQUIRE(document.skins.size() == 1);
        CHECK(document.skins[0].skeleton.name == "Bar skin");
        CHECK(document.skins[0].joints == std::vector<uint32_t>{1, 2, 3});
        CHECK(document.skins[0].skeleton.joints[1].name == "Middle");
        CHECK(document.nodes[0].skin == 0u);
        REQUIRE(document.animations.size() == 2);
        CHECK(document.animations[0].animation.name == "Bend");
        CHECK(document.animations[1].animation.name == "Sway");
        REQUIRE(document.animations[1].animation.tracks.size() == 1);
        CHECK(document.animations[1].animation.tracks[0].joint == "Root");
        CHECK(document.animations[1].animation.tracks[0].translation.empty());
        CHECK(document.animations[1].nodes == std::vector<uint32_t>{1});
    }

    TEST_CASE("documents whose skins or animations are not consistent are refused, and nothing is written") {
        const TempDirectory directory;
        const std::filesystem::path path = directory.path() / "refused.gltf";
        const auto refused = [&](const GltfDocument& document, std::string_view because) {
            const Status status = exportGltf(document, path);
            REQUIRE_FALSE(status.has_value());
            const std::string& message = status.error().message;
            CHECK_MESSAGE(message.find(because) != std::string::npos, message);
            CHECK_FALSE(std::filesystem::exists(path));
        };

        GltfDocument document = makeSkinnedBar();
        document.skins[0].joints.clear();
        document.skins[0].skeleton.joints.clear();
        refused(document, "skin 'Bar skin' has no joints");

        document = makeSkinnedBar();
        document.skins[0].joints.pop_back();
        refused(document, "names 2 nodes for the 3 joints");
        document = makeSkinnedBar();
        document.skins[0].joints[2] = 1;
        refused(document, "the same node twice");
        document = makeSkinnedBar();
        document.skins[0].joints[2] = 40;
        refused(document, "a node that is not there");
        document = makeSkinnedBar();
        document.skins[0].skeleton.joints[1].parent = 1;
        refused(document, "hangs on itself");

        document = makeSkinnedBar();
        document.nodes[0].skin = 3;
        refused(document, "node 'Bar' refers to a skin that is not there");
        document = makeSkinnedBar();
        document.nodes[1].skin = 0;
        refused(document, "has a skin and no mesh");
        document = makeSkinnedBar();
        document.meshes[0].mesh.skin.clear();
        refused(document, "does not have the joints its vertices follow");
        document = makeSkinnedBar();
        document.meshes[0].mesh.skin[0].joints[0] = 7;
        refused(document, "does not have the joints its vertices follow");

        document = makeSkinnedBar();
        document.animations[0].nodes.clear();
        refused(document, "animation 'Bend' names 0 nodes for its 1 tracks");
        document = makeSkinnedBar();
        document.animations[0].nodes[0] = 9;
        refused(document, "animation 'Bend' moves node 9");
        document = makeSkinnedBar();
        document.animations[0].animation.tracks[0].rotation.clear();
        refused(document, "animation 'Bend' has no keys");
        document = makeSkinnedBar();
        document.animations[0].animation.duration = 0.25f;
        refused(document, "keys after its end");
        document = makeSkinnedBar();
        document.animations[1].animation.tracks[0].translation.addKey(0.5f, Vec3(2.0f), Interpolation::Linear);
        refused(document, "do not all go on to the next the same way");

        // And as they came, they are written.
        CHECK(exportGltf(makeSkinnedBar(), path).has_value());
        CHECK(std::filesystem::exists(path));
    }

    TEST_CASE("a skeleton is made of the nodes of a document, or refused") {
        const GltfDocument document = makeSkinnedBar();
        const std::vector<uint32_t> joints = {1, 2, 3};
        // Without matrices: bound where they are.
        SkeletonData skeleton = unwrap(skeletonOf(document, joints, {}, "Plain"));
        CHECK(skeleton.name == "Plain");
        CHECK(skeleton.joints[2].inverseBind == Mat4(1.0f));
        CHECK(skeleton.joints[2].parent == 1);
        // Only some of the nodes: what is left out counts as what is in between.
        skeleton = unwrap(skeletonOf(document, std::vector<uint32_t>{3, 1}, {}, "Ends"));
        CHECK(skeleton.joints[0].name == "Tip");
        CHECK(skeleton.joints[0].parent == 1);
        CHECK(skeleton.joints[0].parentOffset[3] == Vec4(0.0f, 1.0f, 0.0f, 1.0f));
        CHECK(unwrap(skeletonOf(document, {}, {}, "None")).joints.empty());

        const std::vector<Mat4> one(1, Mat4(1.0f));
        CHECK_FALSE(skeletonOf(document, joints, one, "Matrices").has_value());
        CHECK_FALSE(skeletonOf(document, std::vector<uint32_t>{1, 1}, {}, "Twice").has_value());
        CHECK_FALSE(skeletonOf(document, std::vector<uint32_t>{1, 4}, {}, "Beyond").has_value());
        const std::vector<uint32_t> tooMany(kMaxSkeletonJoints + 1, 1);
        const Result<SkeletonData> result = skeletonOf(document, tooMany, {}, "Many");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::OutOfRange);

        // Nodes that hang on one another in a circle, in a document made by hand.
        GltfDocument circle = document;
        circle.nodes[1].parent = 3;
        circle.nodes[0].parent = 0;
        CHECK_FALSE(skeletonOf(circle, joints, {}, "Circle").has_value());
        CHECK_FALSE(skeletonOf(circle, std::vector<uint32_t>{0}, {}, "Self").has_value());
        circle.nodes[2].parent = 77;
        CHECK_FALSE(skeletonOf(circle, std::vector<uint32_t>{3}, {}, "Nowhere").has_value());
    }
}
