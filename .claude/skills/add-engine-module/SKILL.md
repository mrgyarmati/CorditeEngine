---
name: add-engine-module
description: Add a new engine module (static library) or a new source file to an existing module in Cordite. Use when creating a subsystem such as physics or audio, when adding .h/.cpp files, or when the module_layering test fails.
---

# Add an engine module or source file

## Adding a file to an existing module

1. Create `engine/cordite/<module>/<Thing>.h` and `.cpp`. One main type per file, the
   file named after it.
2. Start the header with `#pragma once` and a `/// @file` comment that explains what the
   file provides and includes a `@code` usage example.
3. Add both files to `SOURCES` in `engine/cordite/<module>/CMakeLists.txt`, keeping the
   list alphabetical.
4. Create `tests/unit/<module>/<Thing>Tests.cpp` and add it to that module's
   `cordite_add_test_suite` call in `tests/CMakeLists.txt`.
5. Build and test (skill: `build-and-test`).

Sources are listed explicitly rather than globbed so that adding or removing a file is
visible in review and always triggers a reconfigure.

## Adding a module

A module is a static library `cordite::<name>` in `engine/cordite/<name>/`. Create one
when a subsystem has its own third-party dependency, or when other modules should be
able to use it without pulling in everything above it.

1. **Decide its place in the layering.** List the modules it needs. It may not depend
   on a module that depends on it, directly or indirectly. If two modules need each
   other, one of them needs an interface that the other implements.

2. **Create `engine/cordite/<name>/CMakeLists.txt`:**

   ```cmake
   cordite_add_module(<name>
       SOURCES
           Thing.cpp
           Thing.h
       DEPENDS
           core
           math
       PUBLIC_LIBRARIES
           # third-party targets whose types appear in this module's public headers
       PRIVATE_LIBRARIES
           # third-party targets used only in .cpp files
   )
   ```

3. **Register it** in `engine/CMakeLists.txt`. The list is in dependency order; put the
   new module after everything it depends on.

4. **Add a test suite** in `tests/CMakeLists.txt`:

   ```cmake
   cordite_add_test_suite(<name>
       SOURCES
           unit/<name>/ThingTests.cpp
       LIBRARIES
           cordite::<name>)
   ```

   Add `LABELS gpu` if the suite needs a Vulkan device.

5. **Document it:** add a row to the module tables in `AGENTS.md` and
   `docs/architecture.md`.

6. Reconfigure, build and test. The `module_layering` test now also checks the new
   module.

## When `module_layering` fails

The message names the file, its module, and the module it wrongly includes from.

- If the dependency is legitimate and does not create a cycle, add it to `DEPENDS`.
- If it would create a cycle, or lets a low-level module reach up (for example `core`
  including from the renderer), the code is in the wrong place. Move the shared piece
  down into the lower module, or define an interface there and implement it above.
- A directory under `engine/cordite/` that is not a declared module also fails the
  test. Every directory there must have a `cordite_add_module` call.
