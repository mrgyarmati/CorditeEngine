---
name: add-dependency
description: Add or upgrade a third-party library in Cordite. Use when a task needs a new library, when bumping a dependency version, or when a dependency fails to download, verify or build.
---

# Add or upgrade a dependency

## Before adding one

A dependency is a long-term cost. Add it only when all of these hold, and say so in the
commit message:

- It replaces code we would otherwise have to write and maintain, and is the kind of
  thing people expect a library for (parsing, compression, physics, hashing).
- Its license permits static linking into closed-source games (MIT, BSD, zlib, Apache-2.0,
  CC0, public domain). No GPL or LGPL.
- It is actively maintained and has tagged releases.
- It builds from source with a C++ compiler and CMake alone on Windows, Linux and macOS.
  The Cordite build must not start requiring Python, Perl, a package manager or an SDK
  installer. Prebuilt binaries are acceptable only from the official publisher, pinned
  by hash, and only when a from-source build is impractical.

## Steps

1. **Pick an immutable archive.** Prefer an official release asset; otherwise a tag
   archive (`https://github.com/<org>/<repo>/archive/refs/tags/<tag>.tar.gz`); for
   projects without releases, a commit archive
   (`https://github.com/<org>/<repo>/archive/<full-sha>.tar.gz`). Never a branch.

2. **Compute its hash.**

   ```powershell
   ./scripts/hash-url.ps1 <url>
   ```

3. **Declare it** in `cmake/CorditeDependencies.cmake`, next to the dependencies it
   resembles, with a one-line comment saying what it is for:

   ```cmake
   # --- <name>: <purpose> -------------------------------------------------------------
   cordite_declare_dependency(<name>
       URL <url>
       SHA256 <hash>)
   cordite_dependency_option(<NAME>_BUILD_TESTS OFF)
   cordite_dependency_option(<NAME>_INSTALL OFF)
   ```

   - Turn off the dependency's tests, examples, documentation and install rules.
   - Use `SOURCES_ONLY` for single-header libraries or projects without usable CMake,
     then define a small target for it in the same file (see `cordite_xxhash`).
   - Use `SOURCE_SUBDIR <dir>` when the dependency's `CMakeLists.txt` is not at its root.
   - Add the name to a `FetchContent_MakeAvailable(...)` call.

4. **Link it** from the one module that needs it, as a `PRIVATE_LIBRARIES` entry unless
   its types appear in that module's public headers. Keep its headers out of our
   public headers (pimpl or forward declarations) so it can be replaced later.

5. **Reconfigure and build** all three configurations with both compilers:

   ```powershell
   ./scripts/dev.ps1 configure
   ./scripts/dev.ps1 build
   ./scripts/dev.ps1 build -Config Release
   ./scripts/dev.ps1 build -Config Debug
   ./scripts/dev.ps1 configure -Preset windows-clang
   ./scripts/dev.ps1 build -Preset windows-clang
   ```

   Warnings inside the dependency are not our problem (it is a `SYSTEM` include), but
   warnings it causes in our code are.

6. **Document it** in `docs/third-party.md`: name, version, license and purpose.

7. **Test the wrapper**, not the library: unit tests cover the Cordite API built on top
   of it, including its failure paths.

## Upgrading

Change the URL and hash together, reconfigure, rebuild everything and run the full test
suite. Read the dependency's release notes for behavior changes that our tests would
not notice. Update the version in `docs/third-party.md`.

## Troubleshooting

- **Hash mismatch on a fresh download:** do not paste in the new hash. An archive that
  changes under a fixed URL is either a re-tagged release or a compromised one. Check
  upstream, then pin to a commit archive instead.
- **`cmake_minimum_required` too old for CMake 4:** already handled globally by
  `CMAKE_POLICY_VERSION_MINIMUM` in `CorditeDependencies.cmake`.
- **The dependency overrides the MSVC runtime:** everything must use the static runtime
  (`/MT`, `/MTd`). Find the option that controls it and set it explicitly.
- **It needs a generator tool at build time:** look for an option that uses pregenerated
  sources, or choose a different library.
