#include "cordite/editor/AssetBrowser.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

using Lines = std::vector<std::string>;

/// A project to browse:
///
///     materials/paint.material.json
///     models/cart.gltf              a model with two meshes and a material inside
///     scenes/yard.scene.json
///     scripts/gate.lua
///     textures/old/rust.png
///     textures/wall.png
///     top.lua                       at the top, in no directory
///     readme.txt                    no asset
struct BrowserProject {
    TempDirectory directory;
    std::filesystem::path root = directory.path() / "assets";
    std::unique_ptr<AssetManager> assets;

    BrowserProject() {
        // The database tells an asset by the name of its file: what is in these is
        // nobody's business here.
        write("textures/wall.png", "");
        write("textures/old/rust.png", "");
        write("materials/paint.material.json", R"({"format": "cordite.material", "version": 1})");
        write("scenes/yard.scene.json", "{}");
        write("scripts/gate.lua", "return {}");
        write("top.lua", "return {}");
        write("readme.txt", "not an asset");
        writeModel("models/cart.gltf");
        assets = unwrap(AssetManager::create({.rootDirectory = root}));
        REQUIRE(database().problems().empty());
    }

    [[nodiscard]] const AssetDatabase& database() const { return assets->database(); }

    void write(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(root / pathFromUtf8(relativePath), text).has_value());
    }

    void writeModel(std::string_view relativePath) const {
        GltfDocument document;
        GltfMaterial paint;
        paint.name = "Paint";
        document.materials.push_back(paint);
        GltfMesh body;
        body.mesh = makeBox(Vec3(2.0f, 1.0f, 4.0f));
        body.mesh.name = "Body";
        body.materials = {0};
        GltfMesh wheel;
        wheel.mesh = makeCylinder(0.5f, 0.3f, 12);
        wheel.mesh.name = "Wheel";
        wheel.materials = {std::nullopt};
        document.meshes = {body, wheel};
        GltfNode bodyNode;
        bodyNode.name = "Body";
        bodyNode.mesh = 0;
        bodyNode.children = {1};
        GltfNode wheelNode;
        wheelNode.name = "Wheel";
        wheelNode.parent = 0;
        wheelNode.mesh = 1;
        document.nodes = {bodyNode, wheelNode};
        document.roots = {0};
        REQUIRE(exportGltf(document, root / pathFromUtf8(relativePath)).has_value());
    }

    [[nodiscard]] Uuid id(std::string_view reference) const {
        const AssetInfo* asset = database().findByReference(reference);
        REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
        return asset->id;
    }
};

/// The rows as text, one line each: indented by how far in the row is, a directory with
/// a slash after its name, an asset with its type; "..." for a row that is closed over
/// what is below it, "-" for a file that is only there for what is inside it, and "*"
/// for the asset that is chosen.
Lines outline(const std::vector<AssetBrowserRow>& rows) {
    Lines lines;
    for (const AssetBrowserRow& row : rows) {
        std::string line(static_cast<size_t>(row.depth) * 2, ' ');
        line += row.label;
        if (row.kind == AssetBrowserRow::Kind::Directory) {
            line += "/";
        } else {
            line += fmt::format(" ({})", nameOf(row.type));
        }
        if (row.hasChildren && !row.open) {
            line += " ...";
        }
        if (row.kind == AssetBrowserRow::Kind::Asset && !row.matches) {
            line += " -";
        }
        if (row.selected) {
            line += " *";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// The row with this path - of an asset, or of a directory.
AssetBrowserRow rowAt(const std::vector<AssetBrowserRow>& rows, std::string_view path) {
    const auto found = std::ranges::find(rows, path, &AssetBrowserRow::path);
    REQUIRE_MESSAGE(found != rows.end(), "no row " << std::string(path));
    return *found;
}

/// What is listed of the project, without what the engine has built in.
Lines projectOutline(const AssetBrowser& browser, const BrowserProject& project) {
    Lines lines = outline(browser.rows(project.database()));
    const auto builtin = std::ranges::find(lines, std::string("builtin/"));
    lines.erase(builtin, lines.end());
    return lines;
}

} // namespace

TEST_SUITE("AssetBrowser") {
    TEST_CASE("the browser lists directories and files in the order of their names, and what is built in last") {
        const BrowserProject project;
        const AssetBrowser browser;
        const std::vector<AssetBrowserRow> rows = browser.rows(project.database());

        // Directories are open, a file with something inside it is closed, and what is
        // no asset is not there.
        CHECK(outline(rows) == Lines{"materials/",
                                     "  paint.material.json (material)",
                                     "models/",
                                     "  cart.gltf (model) ...",
                                     "scenes/",
                                     "  yard.scene.json (scene)",
                                     "scripts/",
                                     "  gate.lua (script)",
                                     "textures/",
                                     "  old/",
                                     "    rust.png (texture)",
                                     "  wall.png (texture)",
                                     "top.lua (script)",
                                     "builtin/",
                                     "  Cube (mesh)",
                                     "  Sphere (mesh)",
                                     "  Plane (mesh)",
                                     "  Cylinder (mesh)",
                                     "  Cone (mesh)",
                                     "  Capsule (mesh)",
                                     "  Torus (mesh)",
                                     "  Default (material)"});

        // A row says which asset it is, and where: what the commands of the editor take.
        const AssetBrowserRow cart = rowAt(rows, "models/cart.gltf");
        CHECK(cart.kind == AssetBrowserRow::Kind::Asset);
        CHECK(cart.asset == project.id("models/cart.gltf"));
        CHECK(cart.type == AssetType::Model);
        CHECK(cart.depth == 1);
        CHECK(cart.key == "models/cart.gltf");
        CHECK(cart.hasChildren);
        CHECK_FALSE(cart.open);
        CHECK(cart.matches);
        const AssetBrowserRow old = rowAt(rows, "textures/old");
        CHECK(old.kind == AssetBrowserRow::Kind::Directory);
        CHECK(old.label == "old");
        CHECK(old.key == "textures/old");
        CHECK(old.asset.isNil());
        CHECK(old.depth == 1);
        CHECK(old.open);
        CHECK_FALSE(old.matches);
        // What has nothing below it is neither open nor closed, and has no key to say so by.
        const AssetBrowserRow wall = rowAt(rows, "textures/wall.png");
        CHECK_FALSE(wall.hasChildren);
        CHECK_FALSE(wall.open);
        CHECK(wall.key.empty());
        CHECK(rowAt(rows, "builtin#mesh/cube").asset == builtinMeshId(BuiltinMesh::Cube));
        CHECK(rowAt(rows, "builtin").kind == AssetBrowserRow::Kind::Directory);
    }

    TEST_CASE("what is inside a file is listed once the file is opened, and a directory can be closed") {
        const BrowserProject project;
        AssetBrowser browser;
        const uint64_t before = browser.revision();

        browser.setOpen(rowAt(browser.rows(project.database()), "models/cart.gltf"), true);
        CHECK(browser.revision() == before + 1);
        // In the order the file has them: its meshes, then its material.
        CHECK(projectOutline(browser, project) ==
              Lines{"materials/", "  paint.material.json (material)", "models/", "  cart.gltf (model)",
                    "    Body (mesh)", "    Wheel (mesh)", "    Paint (material)", "scenes/",
                    "  yard.scene.json (scene)", "scripts/", "  gate.lua (script)", "textures/", "  old/",
                    "    rust.png (texture)", "  wall.png (texture)", "top.lua (script)"});
        const std::vector<AssetBrowserRow> opened = browser.rows(project.database());
        CHECK(rowAt(opened, "models/cart.gltf").open);
        CHECK(rowAt(opened, "models/cart.gltf#mesh/1").depth == 2);
        CHECK(rowAt(opened, "models/cart.gltf#mesh/1").asset == project.id("models/cart.gltf#mesh/1"));

        // A directory that is closed hides everything below it, directories included.
        browser.setOpen(rowAt(opened, "textures"), false);
        browser.setOpen(rowAt(opened, "models"), false);
        CHECK(browser.revision() == before + 3);
        CHECK(projectOutline(browser, project) == Lines{"materials/", "  paint.material.json (material)", "models/ ...",
                                                        "scenes/", "  yard.scene.json (scene)", "scripts/",
                                                        "  gate.lua (script)", "textures/ ...", "top.lua (script)"});
        // Opened again, the file in it is as it was left: open.
        browser.setOpen(rowAt(browser.rows(project.database()), "models"), true);
        CHECK(rowAt(browser.rows(project.database()), "models/cart.gltf").open);

        // Asking for what is so already changes nothing, and neither does a row that has
        // nothing below it.
        const uint64_t settled = browser.revision();
        browser.setOpen(rowAt(browser.rows(project.database()), "models"), true);
        browser.setOpen(rowAt(browser.rows(project.database()), "top.lua"), true);
        CHECK(browser.revision() == settled);
        CHECK_FALSE(browser.isOpen(rowAt(browser.rows(project.database()), "top.lua")));

        // What is built in is a directory like the others.
        browser.setOpen(rowAt(browser.rows(project.database()), "builtin"), false);
        CHECK(outline(browser.rows(project.database())).back() == "builtin/ ...");
    }

    TEST_CASE("a filter by type lists what is of that type, with where it is") {
        const BrowserProject project;
        AssetBrowser browser;

        // The model is not a mesh: it is there, marked, for the meshes inside it - and open.
        browser.setFilter({.type = AssetType::Mesh});
        CHECK(outline(browser.rows(project.database())) ==
              Lines{"models/", "  cart.gltf (model) -", "    Body (mesh)", "    Wheel (mesh)", "builtin/",
                    "  Cube (mesh)", "  Sphere (mesh)", "  Plane (mesh)", "  Cylinder (mesh)", "  Cone (mesh)",
                    "  Capsule (mesh)", "  Torus (mesh)"});

        // The model itself: what is inside it is no model, and is not listed.
        browser.setFilter({.type = AssetType::Model});
        const std::vector<AssetBrowserRow> models = browser.rows(project.database());
        CHECK(outline(models) == Lines{"models/", "  cart.gltf (model)"});
        CHECK_FALSE(models[1].hasChildren);
        CHECK(models[1].key.empty());

        browser.setFilter({.type = AssetType::Material});
        CHECK(outline(browser.rows(project.database())) ==
              Lines{"materials/", "  paint.material.json (material)", "models/", "  cart.gltf (model) -",
                    "    Paint (material)", "builtin/", "  Default (material)"});
        browser.setFilter({.type = AssetType::Texture});
        CHECK(outline(browser.rows(project.database())) ==
              Lines{"textures/", "  old/", "    rust.png (texture)", "  wall.png (texture)"});
        browser.setFilter({.type = AssetType::Script});
        CHECK(outline(browser.rows(project.database())) ==
              Lines{"scripts/", "  gate.lua (script)", "top.lua (script)"});
        // A type the project has nothing of.
        browser.setFilter({.type = AssetType::Sound});
        CHECK(browser.rows(project.database()).empty());
    }

    TEST_CASE("a filter by text looks at names and at paths, whatever the case") {
        const BrowserProject project;
        AssetBrowser browser;
        const auto searched = [&](std::string_view text) {
            browser.setFilter({.search = std::string(text)});
            return outline(browser.rows(project.database()));
        };

        CHECK(searched("WALL") == Lines{"textures/", "  wall.png (texture)"});
        // In the name of a directory, which is part of the path of what is in it.
        CHECK(searched("old") == Lines{"textures/", "  old/", "    rust.png (texture)"});
        // In the name an asset has inside its file - and blanks around the text do not count.
        CHECK(searched("  paint ") == Lines{"materials/", "  paint.material.json (material)", "models/",
                                            "  cart.gltf (model) -", "    Paint (material)"});
        // The file and what is inside it: the path of the insides begins with that of the file.
        CHECK(searched("cart") ==
              Lines{"models/", "  cart.gltf (model)", "    Body (mesh)", "    Wheel (mesh)", "    Paint (material)"});
        CHECK(searched(".lua") == Lines{"scripts/", "  gate.lua (script)", "top.lua (script)"});
        CHECK(searched("cube") == Lines{"builtin/", "  Cube (mesh)"});
        CHECK(searched("no such thing").empty());
        // Nothing but blanks is no text to look for, and no filter: nothing is left out,
        // and nothing is opened for it.
        CHECK(searched("   ") == outline(AssetBrowser().rows(project.database())));
        CHECK_FALSE(browser.filter().isActive());
        CHECK(browser.filter().search.empty());
        browser.setFilter({.search = " gate\t"});
        CHECK(browser.filter().search == "gate");

        // Both at once: of the type, and with the text.
        browser.setFilter({.type = AssetType::Mesh, .search = "c"});
        CHECK(outline(browser.rows(project.database())) ==
              Lines{"models/", "  cart.gltf (model) -", "    Body (mesh)", "    Wheel (mesh)", "builtin/",
                    "  Cube (mesh)", "  Cylinder (mesh)", "  Cone (mesh)", "  Capsule (mesh)"});
    }

    TEST_CASE("everything is open while a filter leaves something out, and as it was left afterwards") {
        const BrowserProject project;
        AssetBrowser browser;
        browser.setOpen(rowAt(browser.rows(project.database()), "textures"), false);
        const Lines before = outline(browser.rows(project.database()));
        const uint64_t revision = browser.revision();

        CHECK_FALSE(browser.filter().isActive());
        browser.setFilter({.search = "rust"});
        CHECK(browser.filter().isActive());
        CHECK(browser.revision() == revision + 1);
        // The directory that was closed is open: what was asked for is to be seen.
        CHECK(outline(browser.rows(project.database())) == Lines{"textures/", "  old/", "    rust.png (texture)"});
        CHECK(browser.isOpen(rowAt(browser.rows(project.database()), "textures")));
        // What has nothing below it is not open for that: there is nothing to show.
        const AssetBrowserRow rust = rowAt(browser.rows(project.database()), "textures/old/rust.png");
        CHECK_FALSE(rust.open);
        CHECK_FALSE(browser.isOpen(rust));

        // The same filter again is no change; none at all brings back what was there.
        browser.setFilter({.search = "rust"});
        CHECK(browser.revision() == revision + 1);
        browser.setFilter({});
        CHECK(browser.revision() == revision + 2);
        CHECK(outline(browser.rows(project.database())) == before);
        CHECK(AssetBrowserFilter{.type = AssetType::Mesh}.isActive());
    }

    TEST_CASE("the asset that is chosen is marked where it is listed, and stays chosen where it is not") {
        BrowserProject project;
        AssetBrowser browser;
        const Uuid wheel = project.id("models/cart.gltf#mesh/1");
        const uint64_t before = browser.revision();
        CHECK(browser.selection().isNil());
        // No asset is chosen: no row is marked, not even the directories, which have none.
        CHECK(std::ranges::none_of(browser.rows(project.database()), &AssetBrowserRow::selected));

        browser.select(wheel);
        CHECK(browser.selection() == wheel);
        CHECK(browser.revision() == before + 1);
        browser.select(wheel);
        CHECK(browser.revision() == before + 1);
        // Inside a file that is closed: chosen, and not to be seen.
        CHECK(std::ranges::none_of(browser.rows(project.database()), &AssetBrowserRow::selected));
        browser.setOpen(rowAt(browser.rows(project.database()), "models/cart.gltf"), true);
        CHECK(projectOutline(browser, project)[5] == "    Wheel (mesh) *");
        CHECK(std::ranges::count_if(browser.rows(project.database()), &AssetBrowserRow::selected) == 1);

        // The assets are read again - with a file more, and one less: the rows are those
        // of what is there now, and what is chosen is chosen still.
        project.write("scripts/alarm.lua", "return {}");
        REQUIRE(std::filesystem::remove(project.root / "top.lua"));
        REQUIRE(project.assets->refresh().has_value());
        CHECK(projectOutline(browser, project) ==
              Lines{"materials/", "  paint.material.json (material)", "models/", "  cart.gltf (model)",
                    "    Body (mesh)", "    Wheel (mesh) *", "    Paint (material)", "scenes/",
                    "  yard.scene.json (scene)", "scripts/", "  alarm.lua (script)", "  gate.lua (script)", "textures/",
                    "  old/", "    rust.png (texture)", "  wall.png (texture)"});

        // An asset that is gone stays chosen too - it may come back - and marks nothing.
        const Uuid gate = project.id("scripts/gate.lua");
        browser.select(gate);
        REQUIRE(std::filesystem::remove(project.root / "scripts" / "gate.lua"));
        REQUIRE(project.assets->refresh().has_value());
        CHECK(browser.selection() == gate);
        CHECK(std::ranges::none_of(browser.rows(project.database()), &AssetBrowserRow::selected));
        browser.select(Uuid());
        CHECK(browser.selection().isNil());
    }

    TEST_CASE("an asset is revealed by opening what it is in") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        // Closed: the directory, and the file inside it.
        browser.setOpen(rowAt(browser.rows(database), "models"), false);
        const Uuid wheel = project.id("models/cart.gltf#mesh/1");
        const uint64_t before = browser.revision();

        CHECK(browser.reveal(database, wheel));
        CHECK(browser.revision() == before + 2);
        CHECK(projectOutline(browser, project) ==
              Lines{"materials/", "  paint.material.json (material)", "models/", "  cart.gltf (model)",
                    "    Body (mesh)", "    Wheel (mesh)", "    Paint (material)", "scenes/",
                    "  yard.scene.json (scene)", "scripts/", "  gate.lua (script)", "textures/", "  old/",
                    "    rust.png (texture)", "  wall.png (texture)", "top.lua (script)"});
        // Revealing chooses nothing, and what is to be seen already is left as it is.
        CHECK(browser.selection().isNil());
        CHECK(browser.reveal(database, wheel));
        CHECK(browser.reveal(database, project.id("top.lua")));
        CHECK(browser.reveal(database, project.id("models/cart.gltf")));
        CHECK(browser.revision() == before + 2);

        // Two directories deep, and only those are opened: the one beside them stays closed.
        browser.setOpen(rowAt(browser.rows(database), "textures/old"), false);
        browser.setOpen(rowAt(browser.rows(database), "textures"), false);
        browser.setOpen(rowAt(browser.rows(database), "scripts"), false);
        CHECK(browser.reveal(database, project.id("textures/old/rust.png")));
        const Lines revealed = projectOutline(browser, project);
        CHECK(std::ranges::find(revealed, std::string("    rust.png (texture)")) != revealed.end());
        CHECK(std::ranges::find(revealed, std::string("scripts/ ...")) != revealed.end());
        // What is built in is in a directory too.
        browser.setOpen(rowAt(browser.rows(database), "builtin"), false);
        CHECK(browser.reveal(database, builtinMeshId(BuiltinMesh::Torus)));
        CHECK(outline(browser.rows(database)).back() == "  Default (material)");

        // What the filter does not list has no row to show, and neither has what is no
        // asset: nothing is opened for them.
        browser.setFilter({.type = AssetType::Texture});
        const uint64_t filtered = browser.revision();
        CHECK_FALSE(browser.reveal(database, wheel));
        CHECK_FALSE(browser.reveal(database, Uuid::fromName(Uuid(), "no such asset")));
        CHECK_FALSE(browser.reveal(database, Uuid()));
        CHECK(browser.revision() == filtered);
    }

    TEST_CASE("a directory of the project that is called builtin is not what the engine has built in") {
        BrowserProject project;
        project.write("builtin/crest.png", "");
        REQUIRE(project.assets->refresh().has_value());
        AssetBrowser browser;
        const AssetDatabase& database = project.database();

        // Two directories of one name: the project's among the others, the engine's last.
        const Lines lines = outline(browser.rows(database));
        CHECK(lines[0] == "builtin/");
        CHECK(lines[1] == "  crest.png (texture)");
        CHECK(lines[2] == "materials/");
        CHECK(lines[lines.size() - 9] == "builtin/");
        CHECK(lines.back() == "  Default (material)");

        // Each is closed for itself: the first row of that name is the project's.
        const std::vector<AssetBrowserRow> rows = browser.rows(database);
        REQUIRE(rows.front().path == "builtin");
        browser.setOpen(rows.front(), false);
        const Lines closed = outline(browser.rows(database));
        CHECK(closed[0] == "builtin/ ...");
        CHECK(closed[1] == "materials/");
        CHECK(closed[closed.size() - 9] == "builtin/");
        CHECK(closed.back() == "  Default (material)");
        // And the other way round.
        browser.setOpen(rows.front(), true);
        browser.setOpen(rows[rows.size() - 9], false);
        const Lines other = outline(browser.rows(database));
        CHECK(other[1] == "  crest.png (texture)");
        CHECK(other.back() == "builtin/ ...");
    }

    TEST_CASE("without a project the browser lists what the engine has built in") {
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        AssetBrowser browser;
        const Lines lines = outline(browser.rows(assets->database()));
        REQUIRE(lines.size() == 9);
        CHECK(lines.front() == "builtin/");
        CHECK(lines.back() == "  Default (material)");
        browser.setFilter({.type = AssetType::Scene});
        CHECK(browser.rows(assets->database()).empty());
    }
}
