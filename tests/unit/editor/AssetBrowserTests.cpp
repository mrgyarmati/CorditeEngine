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

    TEST_CASE("an asset is revealed by showing its folder and opening what it is in") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        // Closed: the directory, and the file inside it.
        browser.setOpen(rowAt(browser.rows(database), "models"), false);
        const Uuid wheel = project.id("models/cart.gltf#mesh/1");
        const uint64_t before = browser.revision();

        // One change, however much it opens.
        CHECK(browser.reveal(database, wheel));
        CHECK(browser.revision() == before + 1);
        CHECK(browser.folder() == "models");
        CHECK(outline(browser.contents(database)) ==
              Lines{"cart.gltf (model)", "  Body (mesh)", "  Wheel (mesh)", "  Paint (material)"});
        CHECK(projectOutline(browser, project) ==
              Lines{"materials/", "  paint.material.json (material)", "models/", "  cart.gltf (model)",
                    "    Body (mesh)", "    Wheel (mesh)", "    Paint (material)", "scenes/",
                    "  yard.scene.json (scene)", "scripts/", "  gate.lua (script)", "textures/", "  old/",
                    "    rust.png (texture)", "  wall.png (texture)", "top.lua (script)"});
        // Revealing chooses nothing, and what is shown already is left as it is.
        CHECK(browser.selection().isNil());
        CHECK(browser.reveal(database, wheel));
        CHECK(browser.reveal(database, project.id("models/cart.gltf")));
        CHECK(browser.revision() == before + 1);
        // A file at the top is in the top.
        CHECK(browser.reveal(database, project.id("top.lua")));
        CHECK(browser.folder() == kTopFolder);
        CHECK(browser.revision() == before + 2);

        // Two directories deep, and only those are opened: the one beside them stays closed.
        browser.setOpen(rowAt(browser.rows(database), "textures/old"), false);
        browser.setOpen(rowAt(browser.rows(database), "textures"), false);
        browser.setOpen(rowAt(browser.rows(database), "scripts"), false);
        CHECK(browser.reveal(database, project.id("textures/old/rust.png")));
        CHECK(browser.folder() == "textures/old");
        const Lines revealed = projectOutline(browser, project);
        CHECK(std::ranges::find(revealed, std::string("    rust.png (texture)")) != revealed.end());
        CHECK(std::ranges::find(revealed, std::string("scripts/ ...")) != revealed.end());
        // In the tree as well, where it is the folder shown.
        CHECK(outline(browser.folders(database)) == Lines{"Assets/", "  materials/", "  models/", "  scenes/",
                                                          "  scripts/", "  textures/", "    old/ *", "  builtin/"});
        // What is built in has a folder of its own.
        browser.setOpen(rowAt(browser.rows(database), "builtin"), false);
        CHECK(browser.reveal(database, builtinMeshId(BuiltinMesh::Torus)));
        CHECK(browser.folder() == kBuiltinFolder);
        CHECK(outline(browser.rows(database)).back() == "  Default (material)");

        // What the filter does not list is not shown, and is given its place all the same:
        // there it is once the filter lists it.
        browser.setFilter({.type = AssetType::Texture});
        CHECK_FALSE(browser.reveal(database, wheel));
        CHECK(browser.folder() == "models");
        CHECK(browser.contents(database).empty());
        browser.setFilter({});
        CHECK(outline(browser.contents(database))[2] == "  Wheel (mesh)");
        // What is no asset has no place, and nothing changes for it.
        const uint64_t revealedLast = browser.revision();
        CHECK_FALSE(browser.reveal(database, Uuid::fromName(Uuid(), "no such asset")));
        CHECK_FALSE(browser.reveal(database, Uuid()));
        CHECK(browser.revision() == revealedLast);
        CHECK(browser.folder() == "models");
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

        // In the tree, each is a folder of its own, shown for itself.
        const std::vector<AssetBrowserRow> folders = browser.folders(database);
        REQUIRE(folders.size() == 9);
        CHECK(folders[1].label == "builtin");
        CHECK(folders[1].key == "builtin");
        CHECK(folders.back().label == "builtin");
        CHECK(folders.back().key == kBuiltinFolder);
        browser.showFolder(folders[1].key);
        CHECK(outline(browser.contents(database)) == Lines{"crest.png (texture)"});
        browser.showFolder(folders.back().key);
        CHECK(browser.contents(database).size() == 8);
        // A path finds the project's: it is found first.
        CHECK(assetFolderAt(database, "builtin") == "builtin");
        // And the top has both, the engine's last.
        browser.showFolder(kTopFolder);
        const std::vector<AssetBrowserRow> top = browser.contents(database);
        CHECK(top.front().key == "builtin");
        CHECK(top[top.size() - 2].key == kBuiltinFolder);
    }

    TEST_CASE("the tree of folders is the top, its directories in the order of their names, and what is built in") {
        BrowserProject project;
        AssetBrowser browser;
        // The top is shown at first, and every folder is open.
        CHECK(browser.folder() == kTopFolder);
        const std::vector<AssetBrowserRow> folders = browser.folders(project.database());
        CHECK(outline(folders) == Lines{"Assets/ *", "  materials/", "  models/", "  scenes/", "  scripts/",
                                        "  textures/", "    old/", "  builtin/"});
        CHECK(folders.front().kind == AssetBrowserRow::Kind::Directory);
        CHECK(folders.front().label == kTopFolderLabel);
        CHECK(folders.front().key == kTopFolder);
        CHECK(folders.front().path.empty());
        CHECK(folders.front().depth == 0);
        // What has folders in it can be opened and closed; the others have nothing below them.
        CHECK(folders.front().hasChildren);
        CHECK(rowAt(folders, "textures").hasChildren);
        const AssetBrowserRow old = rowAt(folders, "textures/old");
        CHECK(old.key == "textures/old");
        CHECK(old.depth == 2);
        CHECK_FALSE(old.hasChildren);
        CHECK_FALSE(old.open);
        CHECK(old.asset.isNil());
        // What the engine has built in, with a key no directory has.
        CHECK(folders.back().key == kBuiltinFolder);
        CHECK(folders.back().path == kBuiltinAssetPath);
        CHECK(folders.back().depth == 1);

        // A directory is a folder whether assets are in it or not - one that was just made has
        // none - but for one whose name begins with a dot, which is no part of the assets.
        std::filesystem::create_directories(project.root / "empty" / "deeper");
        std::filesystem::create_directories(project.root / ".scratch" / "notes");
        project.write("deep/down/below/note.lua", "return {}");
        REQUIRE(project.assets->refresh().has_value());
        CHECK(outline(browser.folders(project.database())) ==
              Lines{"Assets/ *", "  deep/", "    down/", "      below/", "  empty/", "    deeper/", "  materials/",
                    "  models/", "  scenes/", "  scripts/", "  textures/", "    old/", "  builtin/"});
        // Without a project, there is what the engine has built in.
        const std::unique_ptr<AssetManager> bare = unwrap(AssetManager::create());
        CHECK(outline(browser.folders(bare->database())) == Lines{"Assets/ *", "  builtin/"});
    }

    TEST_CASE("a folder of the tree is opened and closed for itself, and the filter leaves the tree as it is") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        browser.setOpen(rowAt(browser.folders(database), "textures"), false);
        CHECK(outline(browser.folders(database)) == Lines{"Assets/ *", "  materials/", "  models/", "  scenes/",
                                                          "  scripts/", "  textures/ ...", "  builtin/"});
        // It is one directory, closed in rows() as well.
        const Lines rows = outline(browser.rows(database));
        CHECK(std::ranges::find(rows, std::string("textures/ ...")) != rows.end());

        // A filter opens everything in rows(), and nothing in the tree - in which a folder
        // is still opened and closed.
        browser.setFilter({.search = "rust"});
        CHECK(outline(browser.folders(database))[5] == "  textures/ ...");
        browser.setOpen(rowAt(browser.folders(database), "textures"), true);
        CHECK(outline(browser.folders(database))[6] == "    old/");
        // The top closes over everything.
        browser.setOpen(browser.folders(database).front(), false);
        CHECK(outline(browser.folders(database)) == Lines{"Assets/ ... *"});
        browser.setOpen(browser.folders(database).front(), true);
        CHECK(browser.folders(database).size() == 8);
    }

    TEST_CASE("a folder has its directories, then its files, and what is inside a file once it is opened") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        // The top has what the engine has built in among its directories, the last of them.
        CHECK(outline(browser.contents(database)) ==
              Lines{"materials/", "models/", "scenes/", "scripts/", "textures/", "builtin/", "top.lua (script)"});

        browser.showFolder("models");
        CHECK(outline(browser.contents(database)) == Lines{"cart.gltf (model) ..."});
        const AssetBrowserRow cart = browser.contents(database).front();
        CHECK(cart.key == "models/cart.gltf");
        CHECK(cart.hasChildren);
        CHECK(cart.asset == project.id("models/cart.gltf"));
        browser.setOpen(cart, true);
        CHECK(outline(browser.contents(database)) ==
              Lines{"cart.gltf (model)", "  Body (mesh)", "  Wheel (mesh)", "  Paint (material)"});

        // A directory is shown, not opened: nothing is below it here.
        browser.showFolder("textures");
        CHECK(outline(browser.contents(database)) == Lines{"old/", "wall.png (texture)"});
        const AssetBrowserRow old = browser.contents(database).front();
        CHECK(old.kind == AssetBrowserRow::Kind::Directory);
        CHECK(old.key == "textures/old");
        CHECK(old.path == "textures/old");
        CHECK_FALSE(old.hasChildren);
        CHECK(isMovableDirectory(old));

        // What the engine has built in is what its folder has, in no file.
        browser.showFolder(kBuiltinFolder);
        CHECK(outline(browser.contents(database)) == Lines{"Cube (mesh)", "Sphere (mesh)", "Plane (mesh)",
                                                           "Cylinder (mesh)", "Cone (mesh)", "Capsule (mesh)",
                                                           "Torus (mesh)", "Default (material)"});
        // The chosen asset is marked.
        browser.select(builtinMeshId(BuiltinMesh::Sphere));
        CHECK(outline(browser.contents(database))[1] == "Sphere (mesh) *");
    }

    TEST_CASE("a search shows what it finds in the folder and below it, side by side") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        // At the top, everywhere - what is built in too - and what is inside a closed file.
        browser.setFilter({.type = AssetType::Mesh});
        CHECK(outline(browser.contents(database)) == Lines{"Body (mesh)", "Wheel (mesh)", "Cube (mesh)",
                                                           "Sphere (mesh)", "Plane (mesh)", "Cylinder (mesh)",
                                                           "Cone (mesh)", "Capsule (mesh)", "Torus (mesh)"});
        // In a folder, what is in it and below it.
        browser.showFolder("models");
        CHECK(outline(browser.contents(database)) == Lines{"Body (mesh)", "Wheel (mesh)"});
        browser.showFolder(kBuiltinFolder);
        CHECK(browser.contents(database).size() == 7);
        browser.showFolder("textures");
        CHECK(browser.contents(database).empty());
        // By text, whatever its case, two directories down.
        browser.setFilter({.search = "RUST"});
        CHECK(outline(browser.contents(database)) == Lines{"rust.png (texture)"});
        const AssetBrowserRow rust = browser.contents(database).front();
        CHECK(rust.path == "textures/old/rust.png");
        CHECK(rust.depth == 0);
        CHECK(rust.matches);
        browser.showFolder("scripts");
        CHECK(browser.contents(database).empty());
        // A file that is open shows nothing under what it finds: all of it is side by side.
        browser.setFilter({});
        browser.showFolder("models");
        browser.setOpen(rowAt(browser.contents(database), "models/cart.gltf"), true);
        browser.setFilter({.search = "cart"});
        CHECK(outline(browser.contents(database)) ==
              Lines{"cart.gltf (model)", "Body (mesh)", "Wheel (mesh)", "Paint (material)"});
    }

    TEST_CASE("a folder that is gone shows the nearest folder above it, and is shown again when it is back") {
        BrowserProject project;
        AssetBrowser browser;
        browser.showFolder("textures/old");
        CHECK(browser.shownFolder(project.database()) == "textures/old");
        // Deleted, or moved away: a folder emptied of assets is still one while it is there.
        REQUIRE(std::filesystem::remove_all(project.root / "textures" / "old") > 0);
        REQUIRE(project.assets->refresh().has_value());
        // It stays the folder, and the one above it is shown meanwhile.
        CHECK(browser.folder() == "textures/old");
        CHECK(browser.shownFolder(project.database()) == "textures");
        CHECK(outline(browser.contents(project.database())) == Lines{"wall.png (texture)"});
        CHECK(outline(browser.folders(project.database()))[5] == "  textures/ *");

        project.write("textures/old/rust.png", "");
        REQUIRE(project.assets->refresh().has_value());
        CHECK(browser.shownFolder(project.database()) == "textures/old");
        // What was never a folder shows the nearest above it that is: a file is no folder.
        browser.showFolder("models/cart.gltf");
        CHECK(browser.shownFolder(project.database()) == "models");
        browser.showFolder("nowhere/at/all");
        CHECK(browser.shownFolder(project.database()) == kTopFolder);
        // Written as no path is, it is no folder either.
        browser.showFolder("/textures");
        CHECK(browser.shownFolder(project.database()) == kTopFolder);
        browser.showFolder(kBuiltinFolder);
        CHECK(browser.shownFolder(project.database()) == kBuiltinFolder);
    }

    TEST_CASE("showing a folder opens the folders above it, and is one change") {
        const BrowserProject project;
        AssetBrowser browser;
        const AssetDatabase& database = project.database();
        browser.setOpen(rowAt(browser.folders(database), "textures"), false);
        browser.setOpen(browser.folders(database).front(), false);
        const uint64_t before = browser.revision();

        browser.showFolder("textures/old");
        CHECK(browser.revision() == before + 1);
        CHECK(outline(browser.folders(database)) == Lines{"Assets/", "  materials/", "  models/", "  scenes/",
                                                          "  scripts/", "  textures/", "    old/ *", "  builtin/"});
        browser.showFolder("textures/old");
        CHECK(browser.revision() == before + 1);
        // Not the folder itself: what is in it is not asked for.
        browser.setOpen(rowAt(browser.folders(database), "textures"), false);
        browser.showFolder("textures");
        CHECK(outline(browser.folders(database))[5] == "  textures/ ... *");
        browser.showFolder(kBuiltinFolder);
        CHECK(browser.folders(database).back().selected);
        CHECK_FALSE(browser.folders(database).front().selected);
    }

    TEST_CASE("the way to a folder is the top, and each directory it is in down to it") {
        const AssetFolderStep top{.label = "Assets", .key = "", .path = ""};
        CHECK(folderSteps(kTopFolder) == std::vector<AssetFolderStep>{top});
        const AssetFolderStep textures{.label = "textures", .key = "textures", .path = "textures"};
        const AssetFolderStep old{.label = "old", .key = "textures/old", .path = "textures/old"};
        CHECK(folderSteps("textures/old") == std::vector<AssetFolderStep>{top, textures, old});
        // What the engine has built in is in the top, and is known by a key no path has.
        const AssetFolderStep builtin{.label = "builtin", .key = "#builtin", .path = "builtin"};
        CHECK(folderSteps(kBuiltinFolder) == std::vector<AssetFolderStep>{top, builtin});
    }

    TEST_CASE("a folder is found by its path, whatever is listed, and nothing else is a folder") {
        BrowserProject project;
        AssetBrowser browser;
        browser.setFilter({.type = AssetType::Scene});
        CHECK(assetFolderAt(project.database(), "") == std::string(kTopFolder));
        CHECK(assetFolderAt(project.database(), "textures/old") == "textures/old");
        CHECK(assetFolderAt(project.database(), "builtin") == std::string(kBuiltinFolder));
        for (const std::string_view path :
             {"textures/wall.png", "nowhere", "textures/", "/textures", "TEXTURES", "#builtin", "models/cart.gltf"}) {
            CAPTURE(std::string(path));
            CHECK_FALSE(assetFolderAt(project.database(), path).has_value());
        }
        // A directory with no asset in it is a folder; one whose name begins with a dot is none.
        std::filesystem::create_directories(project.root / "empty");
        std::filesystem::create_directories(project.root / ".scratch");
        REQUIRE(project.assets->refresh().has_value());
        CHECK(assetFolderAt(project.database(), "empty") == "empty");
        CHECK_FALSE(assetFolderAt(project.database(), ".scratch").has_value());
    }

    TEST_CASE("the contents are shown as a grid at first, or as a list, by name") {
        AssetBrowser browser;
        CHECK(browser.view() == AssetBrowserView::Grid);
        // What is listed is the same either way.
        const uint64_t before = browser.revision();
        browser.setView(AssetBrowserView::List);
        CHECK(browser.view() == AssetBrowserView::List);
        CHECK(browser.revision() == before);

        CHECK(nameOf(AssetBrowserView::List) == "list");
        CHECK(nameOf(AssetBrowserView::Grid) == "grid");
        for (const AssetBrowserView view : kAssetBrowserViews) {
            CHECK(assetBrowserViewFromName(nameOf(view)) == view);
        }
        CHECK(assetBrowserViewFromName("Grid") == AssetBrowserView::Grid);
        CHECK(assetBrowserViewFromName("LIST") == AssetBrowserView::List);
        CHECK_FALSE(assetBrowserViewFromName("tiles").has_value());
        CHECK_FALSE(assetBrowserViewFromName("").has_value());
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
