# Installing and building

[← README](../README.md) · [по-русски](install.ru.md)

Windows only for now; there is a [note on Linux](#linux) at the bottom saying
what stands in the way and what it would take.

The short version, if the tools are already there:

```
git clone https://github.com/Hiro-zzz/BlockyEngine.git
cd BlockyEngine
build.cmd release
build\RelWithDebInfo\konstruct.exe
```

---

## What you need

**Visual Studio, or just its Build Tools.** The Build Tools are a free download
and are all this needs — the IDE is not used. Get either from
[visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/);
the Build Tools are near the bottom under *Tools for Visual Studio*.

In the installer, tick **Desktop development with C++**. That is the whole
answer and it is what most people should do.

If you would rather install as little as possible, three components are enough:

| Component | Why |
|---|---|
| MSVC v145 (or newer) — x64/x86 build tools | The compiler. Needs to be **14.5 or newer**: the code is C++20 and uses parts of it earlier toolsets get wrong |
| Windows 11 SDK | Headers for Win32 and OpenGL |
| C++ CMake tools for Windows | Brings CMake and Ninja. `build.cmd` uses the ones bundled here rather than any on PATH |

Nothing else. No CMake to install separately, no Ninja, no package manager, no
dependencies to fetch — the engine has none.

**A machine that was verified against**, for reference rather than as a
requirement: MSVC 14.50.35717, Windows SDK 10.0.26100, the bundled CMake 4.2.3
and Ninja 1.12.1, Windows 11.

---

## Getting the source

```
git clone https://github.com/Hiro-zzz/BlockyEngine.git
cd BlockyEngine
```

A clone is about fifteen megabytes, most of which is the gallery under
`docs/gallery/`. There are no submodules and nothing is fetched at build time.

---

## Building

```
build.cmd release
```

That is the whole of it. The script finds Visual Studio itself, brings up the
x64 environment, configures with CMake and Ninja and builds everything: the
engine as a static library, forty-three scenes as separate executables, and the
game as one.

| Command | What it does |
|---|---|
| `build.cmd` | Configure if needed, then build Debug |
| `build.cmd release` | Build RelWithDebInfo — **the only one worth rendering with** |
| `build.cmd run <name>` | Build, then run `scenes/<name>.cpp` from the project root |
| `build.cmd clean` | Delete the `build/` directory |

Debug builds path-trace roughly twenty times slower. They are for stepping
through a solver, not for making a picture.

The first build compiles everything and takes a few minutes; after that Ninja
rebuilds only what changed. Adding or deleting a file under `scenes/`,
`engine/` or `game/` re-runs CMake by itself — the globs are declared
`CONFIGURE_DEPENDS`, so a new scene needs no edit anywhere to become an
executable.

### If Visual Studio is somewhere unusual

`build.cmd` asks `vswhere` — the tool Microsoft ships for this and installs
alongside every Visual Studio since 2017 — and falls back to the usual
directories. If yours is somewhere neither finds, say so:

```
set "BLOCKY_VSROOT=D:\VS\BuildTools"
build.cmd release
```

### Arguments

`build.cmd` passes none through. Run the executable directly when a scene takes
some:

```
build\RelWithDebInfo\scenes\scene_diorama.exe draft
```

---

## Running something

**The game.**

```
build\RelWithDebInfo\konstruct.exe
```

It opens on a menu standing over a real, slowly turning world. `konstruct
map=be_construct` skips the menu and drops you on the build site;
`konstruct physgun` starts with the tool in hand. The full argument list is in
the [README](../README.md#the-game).

**A frame.** Every file in `scenes/` is its own executable named
`scene_<file>`. They write into `out/`, relative to wherever you run them, so
run them from the project root:

```
build.cmd release run cornell
build\RelWithDebInfo\scenes\scene_diorama.exe draft
```

`draft` is accepted by most of them: a smaller, faster pass for judging light
before committing to the real one.

**The tests.** Seventeen suites, each an ordinary scene that returns non-zero
on failure:

```powershell
foreach ($t in @("test_raycast","test_zip","test_assets","test_entity","test_gl",
                 "test_gpu","test_procgen","test_sprite","test_prop","test_rig",
                 "test_face","test_anim","test_video","test_chunks","test_physics",
                 "test_script","hello")) {
    & ".\build\RelWithDebInfo\scenes\scene_$t.exe"
    if ($LASTEXITCODE -ne 0) { Write-Output "FAILED: $t" }
}
```

`test_zip` and `test_assets` need a Minecraft installation and report that they
are skipping rather than failing without one. What each suite covers is in
[testing.md](testing.md).

---

## Optional: Minecraft assets

**Nothing here requires them.** Block textures are generated in code,
characters are drawn in code, the font has a built-in fallback. Every scene
renders and the game plays with none of it installed.

What an installation adds is the real artwork. If you have one, the engine
finds the client `.jar` by itself — `AssetSource::findClientJar()` looks under
`%APPDATA%` for the official launcher, TLauncher and Prism, and takes the
newest version it finds. Nothing is unpacked; the `.jar` is read where it lies.

A resource pack works the same way: hand `AssetSource::open` the `.zip`.

Without one, `scene_textured_island` renders in a flat palette, `test_zip` and
`test_assets` skip themselves, and the game generates its own textures and
notices no difference.

---

## The graphics card

The path tracer on the processor needs nothing at all.

The viewport, the game and the GPU tracer need **OpenGL 4.6 core**. That is
every discrete card from about 2015 and Intel integrated graphics from roughly
the same era, but it does mean current drivers rather than the ones Windows
installs on its own. `scene_test_gl` answers the question directly: it creates
a 4.6 context, checks the function table is complete, and prints what it got.

There is no Vulkan or D3D path and none is planned — the compute shaders the
GPU tracer is built from are core OpenGL 4.3, and the context was already open
for the viewport.

---

## When it does not work

**`no Visual Studio with the C++ toolset was found`** — either it is not
installed, or the C++ workload is not part of what was installed. Re-run the
Visual Studio Installer, hit *Modify*, and tick **Desktop development with
C++**. If it is installed somewhere unusual, set `BLOCKY_VSROOT` as above.

**`found ... but no cmake in it`** — Visual Studio is there but without the
**C++ CMake tools for Windows** component. Same fix: *Modify*, tick it.

**`vcvars64 failed`** — usually a 32-bit-only installation, or one missing the
x64 tools. The component to tick is the one whose name ends *x64/x86 build
tools*.

**The window opens black, or `test_gl` fails.** Graphics drivers. The card must
report OpenGL 4.6; run `scene_test_gl` and it will print what it actually got.

**Cyrillic or other non-ASCII in the path.** Supported deliberately — this
repository is developed under one — but only because every file operation goes
through `readFileBytes` / `writeFileBytes`, which convert to UTF-16 first. The
narrow CRT functions mangle such paths, so if you add code that opens a file,
use those two rather than `fopen`.

**Nothing needs to be installed to run what you built.** The CRT is linked
statically, so the executables import only `OPENGL32`, `USER32`, `GDI32` and
`KERNEL32` — all of which ship with Windows. No redistributable, and a built
scene can be copied to another machine as one file.

---

## Linux

**Not supported yet.** It is wanted and it is planned; what follows is where it
actually stands, because "coming later" on its own tells you nothing about how
far away it is.

Almost all of the engine is ordinary portable C++20 with no dependencies to
port: the path tracer, the physics solver, the PNG and ZIP codecs, the H.264
encoder, the scripting language, the whole of `scenes/`. `engine/core/file.cpp`
already carries a POSIX branch beside its Win32 one.

Two things are genuinely Windows-bound, and they are two files:

| File | What it is | What Linux would need |
|---|---|---|
| `engine/platform/window.cpp` | The window, and raw mouse input, on Win32 | The same `Window` interface over X11 or Wayland |
| `engine/render/gl/gl_loader.cpp` | Function loading through WGL | The same table filled through GLX or EGL |

Plus `build.cmd`, which is a batch script — CMake itself is already neutral
apart from one `if(MSVC)` block of compiler flags.

So the shape of the port is clear: a second implementation behind each of those
two interfaces, and a shell script or a CMake preset beside the `.cmd`. What is
*not* yet true is that anything has been compiled on Linux at all — the
portability above is how the code is written, not something a compiler has
confirmed. Expect the first attempt to turn up a handful of small things,
because it always does.

Until then the honest summary is: Windows 10 or 11, x64.
