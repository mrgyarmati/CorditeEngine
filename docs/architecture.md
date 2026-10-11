# Cordite architecture

This document describes how the engine is put together and why. The working rules
derived from it are in [AGENTS.md](../AGENTS.md).

## Shape of the engine

Cordite is a set of static libraries, one per module, that are linked into two kinds of
executable: the editor, used to build games, and the runtime, which runs an exported
game and contains no editing code.

```
            editor (executable)          runtime (executable)
                      \                    /
                       +---- engine modules (static libraries) ----+
                       |  ...                                      |
                       |  math                                     |
                       |  core                                     |
                       +-------------------------------------------+
```

Modules form a strict layering: a module may use only the modules it lists as
dependencies, and their dependencies. The `module_layering` test enforces this by
scanning `#include` lines, so an accidental upward dependency fails the build instead
of slowly turning the code base into a ball of mud.

### Modules

| Module | Depends on | Responsibility |
|--------|-----------|----------------|
| `core` | -         | Services every other module needs: logging, assertions, error handling, hashing, UUIDs, JSON and binary serialization, file I/O, configuration, the job system, memory allocators, timing, platform queries. |
| `math` | `core`    | glm type aliases and conventions; `Transform`, bounding volumes, rays and intersection tests, frustum culling, projection matrices, color conversions, keyframed curves, deterministic random numbers. |
| `platform` | `core`, `math` | The window system and windows (GLFW), and per-window keyboard, mouse and gamepad state with an injection API for tests and automation. |
| `assets` | `core`, `math` | CPU-side asset data. Currently images: decoding, PNG encoding, pixel access and tolerance-based comparison. |
| `rhi` | `core`, `math`, `platform` | The GPU: Vulkan instance and device creation, the NVRHI device the renderer programs against, the swapchain, and texture readback. The only module that calls Vulkan. |
| `shaders` | `core` | What a shader *is* to the engine: requests and their permutation keys, compiled binaries, the on-disk cache, and the `ShaderLibrary` that resolves a request from memory, disk or a compiler. |
| `shader_compiler` | `core`, `shaders` | The Slang-based implementation of `IShaderCompiler`. Kept separate so exported games do not link a compiler. |
| `renderer` | `core`, `math`, `rhi`, `shaders` | Render passes and the `ShaderManager` that turns shader binaries into GPU shader objects. |
| `engine` | all of the above | `Engine`: creates and owns the subsystems and runs the frame loop. |

Further modules (world, physics, audio, scripting) are added by the later milestones and
will be listed here as they land.

### A frame

`Engine::runFrame()` does the same thing with and without a window:

1. Poll the window system, which starts a new input frame and delivers events.
2. Acquire the swapchain back buffer (windowed mode only). A minimized window yields no
   back buffer and the frame is skipped.
3. Render into the engine's own offscreen frame target.
4. Copy the frame target to the back buffer and present (windowed mode only).
5. Release GPU resources whose last use has finished.

Rendering to an offscreen target first is what makes the engine testable and
automatable: the headless path is not a special case, it is the main path minus step 4.
Screenshots, reference-image tests and editor automation all read the frame target.

## Conventions

- **Coordinates:** right-handed, +Y up, metres. Identity rotation faces -Z.
- **Matrices:** column-major, column vectors, `world = parent * local`.
- **Clip space:** Direct3D-style, x and y in [-1, 1] with +Y up, depth in [0, 1].
  NVRHI flips the Vulkan viewport so the same matrices work on every backend.
- **Depth:** reversed (near = 1, far = 0) for scene rendering, for uniform precision.
- **Angles:** radians in code, degrees in files and user interfaces.
- **Color:** linear RGB in all math; sRGB only at storage and display boundaries.
- **Text:** UTF-8 everywhere, including file paths in data.
- **Identity:** assets and entities are identified by `Uuid`, never by path or name.

## Design decisions

Each entry records a choice that is not obvious from the code, and why it was made.

### Dependencies are fetched as archives pinned by SHA-256

CMake `FetchContent` downloads each dependency from a fixed URL and verifies its hash.
Git branches and tags are mutable, so tracking them would make builds unreproducible
and would let a compromised upstream change what we compile. Archives are also much
faster to fetch than full git histories. Git submodules were rejected because they
need an extra checkout step that is easy to forget, and vcpkg/Conan because they add a
tool every developer and CI machine must install first.

### The build needs only a C++ compiler, CMake and Ninja

No Python, no package manager, no SDK installers. `scripts/bootstrap.ps1` fetches CMake
and Ninja on Windows machines that only have Visual Studio. Dependencies whose build
needs extra tools are avoided.

### The C runtime is linked statically on Windows

Exported games must run on a clean machine without installing the Visual C++
redistributable. Every dependency is built from source with the same setting, so there
is exactly one runtime in the process.

### Expected failures return `Result`, not exceptions

`Result<T>` (an alias of `tl::expected`) makes failure part of a function's signature,
so a caller cannot forget that loading a file may fail. Exceptions remain enabled
because several dependencies use them, but they are caught at the boundary where they
arise and converted. `std::expected` was not used because it needs C++23, which the
three supported standard libraries do not yet implement equally.

### Assertions are for programmer errors only

`CORDITE_ASSERT` disappears in `Release`. Anything that can happen because of data or
user input is an expected failure and returns a `Result`. The assertion handler is
replaceable; tests install one that throws, so failure paths are testable without
killing the test process.

### Logging goes through a small facade over spdlog

Engine code logs with `CORDITE_LOG_*` and never includes spdlog. The facade adds what
the engine needs on top of spdlog's sinks: a category per message, a bounded in-memory
history for the editor console and the automation API, and listeners. Console output
goes to stderr so that stdout stays free for machine-readable output from tools.

### The job system wraps enkiTS

enkiTS is a small, widely used task scheduler. It is hidden behind `JobSystem` with a
pimpl so the rest of the engine depends on three functions (`parallelFor`, `submit`,
`wait`) rather than on a scheduler library. An exception escaping a job is captured and
rethrown to whoever waits, or logged if nobody does, so a failing job is never silent.

### Custom allocators instead of `std::pmr`

`LinearAllocator` and `PoolAllocator` are a few dozen lines each and cover the two
patterns the engine needs: per-frame scratch memory and many same-sized objects.
`std::pmr` would do the same, but its implementation lives in the standard library
binary on macOS and would raise the minimum supported macOS version.

### JSON keeps key order

`Json` is `nlohmann::ordered_json`. Scene, material and settings files are edited by
people and agents and stored in version control, so their layout must be stable and
readable: fields appear in the order the engine writes them rather than alphabetically.

### Deterministic random numbers

`Random` implements PCG32 directly instead of using `<random>`, whose distributions
produce different values on each standard library. Gameplay, procedural content and
image-comparison tests need the same sequence on every platform. The implementation is
verified against the reference generator's published output.

### Vulkan 1.3 is required

NVRHI's Vulkan backend uses dynamic rendering, synchronization2 and timeline semaphores.
All three are core in Vulkan 1.3, which every current desktop driver, MoltenVK and the
lavapipe software rasterizer support. Adapters that lack them are rejected with a
message saying what is missing.

### The application owns the Vulkan device, NVRHI wraps it

NVRHI does not create instances, devices or swapchains; `GraphicsDevice` and `Swapchain`
do, and hand the handles to NVRHI. NVRHI is built with the dynamic Vulkan-Hpp dispatcher
and, as a static library, expects the application to provide and initialize it.

### One GraphicsDevice, one Engine per process

That dispatcher is a single global table of Vulkan entry points, shared by NVRHI and the
`rhi` module. It holds the functions of one instance and one device: extension functions
are only present when that instance or device enabled the extension. Two devices with
different configurations (say, a windowed one and a headless one) would overwrite each
other's entries and crash on the first call that the other did not enable. So
`GraphicsDevice::create` refuses to create a second device while one exists, which also
lets the dispatcher resolve device functions straight to the driver.

GLFW is likewise a process-global library, so `WindowSystem`, and with it `Engine`,
exists at most once at a time. Devices and engines can be created one after another.

### Validation is part of the test contract

Devices created with `validation = true` enable the Khronos validation layer (when
installed) and NVRHI's own validation layer. Everything either reports is recorded on
the `GraphicsDevice`, and every GPU test asserts that the record is empty. A dedicated
test triggers a deliberate error to prove the mechanism detects one. The validation
layers come from a Vulkan SDK that `scripts/bootstrap.ps1 -WithVulkanSdk` installs
inside the repository in copy-only mode, so no system-wide install is needed.

### The swapchain is 8-bit UNORM, not sRGB

The final pass writes display-encoded values itself, and user interface is blended in
display space, as every UI toolkit expects. An sRGB swapchain format would apply the
encoding a second time. Linear HDR rendering happens in offscreen targets.

### Swapchain image layouts are left to NVRHI

A swapchain image may only change layout while it is acquired, and starts out undefined.
NVRHI's state tracker already treats a texture with `keepInitialState` as undefined
until its first use, so the swapchain declares its images with `initialState = Present`
and does nothing else. `present()` additionally requests the Present state, which is free
when the frame rendered into the image and makes an untouched image presentable.

### Shaders are written in Slang and compiled with the official prebuilt compiler

Slang is the shading language the NVRHI ecosystem is built around, compiles HLSL-style
source to SPIR-V on all three platforms, and reports dependencies, which the cache needs.
It is consumed as the official release binaries, pinned by hash: building it from source
would dominate the engine's build time. glslang was rejected because its build needs
Python. The compiler lives in its own module (`shader_compiler`) behind the
`IShaderCompiler` interface so that an exported game links none of it.

Compiler options are passed in `slangc` command-line syntax and parsed by Slang's own
`parseCommandLineArguments`, which avoids depending on the numeric option identifiers of
one particular Slang release.

### Shader bindings use HLSL registers with NVRHI's default offsets

A shader declares `register(t0)`, `register(s0)`, `register(b0)`; the compiler shifts
them to Vulkan bindings 0, 128 and 256 (and 384 for `u`), which is what NVRHI's binding
layouts assume by default. C++ and shader therefore use the same small slot numbers per
resource kind, and the code would carry over to a Direct3D backend unchanged.

### Shader permutations are identified by a content-independent key

A `ShaderRequest` (path, entry point, stage, sorted defines) hashes to a 128-bit key that
names its cache file. The key deliberately excludes file contents and the compiler
version: those are stored *in* the entry and checked on load. One permutation therefore
has exactly one cache file, which is replaced when it goes stale, instead of leaving an
orphan behind for every edit. A test pins the key of a known request, because changing
the key function would silently invalidate every cache.

### Enum names use `nameOf`, not `toString`

doctest stringifies values by calling an unqualified `toString`, which finds any
overload in the value's namespace by argument-dependent lookup and then fails to
compile because the return type is not doctest's string. Using a different verb avoids
the collision without wrapping every enum in the tests.
