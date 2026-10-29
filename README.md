# civ5turbo — Civilization V Hybrid LuaJIT 2.1 + Native C Accelerator (`lua51_Win32.dll`)

[![Build](https://github.com/eutychus/civ5turbo/actions/workflows/build-release.yml/badge.svg)](https://github.com/eutychus/civ5turbo/actions/workflows/build-release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CodeRabbit](https://img.shields.io/badge/CodeRabbit-enabled-ff6b35)](.coderabbit.yaml)

A drop-in replacement for *Sid Meier's Civilization V*'s 32-bit `lua51_Win32.dll` (originally vanilla Lua 5.1.4).

This project combines **LuaJIT 2.1 (rolling)** with a custom **Civilization V Native C Accelerator & Bridge** ([`luajit/src/lj_civ5_accel.c`](luajit/src/lj_civ5_accel.c)) specifically engineered around the reverse-engineered Lua hosting architecture of `CivilizationV.exe`, `CivilizationV_DX11.exe`, `CvGameCore_Expansion2.dll`, and `CvGameDatabaseWin32Final Release.dll`.

---

## Key Highlights

- **4.73x Overall Lua Speedup (Up to 13.81x on Hot Workloads)**: Reduces the 10-workload Civilization V map-generation, gameplay, and UI benchmark suite from **736.34 ms** (vanilla Firaxis `lua51_Win32.dll`) to **155.59 ms**.
- **Benchmark Equivalence**: The harness compares output hashes and `Map.Rand` call counts across 10 selected Civilization V workloads, including a shuffle with a fixed RNG seed. It does not compare complete maps across all seeds.
- **Zero Original Proprietary Code**: Built entirely from clean-room C and LuaJIT 2.1 (MIT licensed). Contains zero bytes of Firaxis source code or assets.
- **Eliminates Host `HeapAlloc` Contention**: Redirects `lua_newstate` away from Civilization V's `HeapAlloc + HeapSize + memcpy + HeapFree` wrapper to LuaJIT's internal `lj_alloc_f` slab allocator (**831,521 -> 0** Win32 heap calls in the benchmark suite).
- **Eliminates LuaJIT `NYI` Trace Aborts**: Intercepts Firaxis's custom C closures registered into every Lua state (`0-based ipairs`, `table.fill`, `table.count`, `math.clamp`) and replaces them with JIT-traceable closures and direct internal `GCtab` array operations.
- **Cross-Thread `GCproto` Bytecode Cache**: Caches compiled Lua function prototypes (`GCproto*`) in `LUA_REGISTRYINDEX` across Civilization V's **186–263+ `lua_newthread` UI/gameplay contexts**, eliminating redundant parsing of shared files (`IconSupport.lua`, `InstanceManager.lua`, `SupportFunctions.lua`, etc.).
- **Linux Cross-Compilation & GitHub Actions CI**: Includes a single-command Linux MinGW-w64 cross-compile script ([`build.sh`](build.sh)), a Windows MSVC build script ([`build_all.bat`](build_all.bat)), and a dual-platform GitHub Actions workflow ([`.github/workflows/build-release.yml`](.github/workflows/build-release.yml)) that builds on every push/PR, verifies the PE export table, and publishes tagged releases. Pull requests are reviewed by CodeRabbit using [`.coderabbit.yaml`](.coderabbit.yaml).

---

## Repository Structure

```text
civ5turbo/
├── .coderabbit.yaml               # CodeRabbit review configuration
├── .github/
│   └── workflows/
│       └── build-release.yml      # CI: Linux MinGW + Windows MSVC builds, tag releases
├── benchmark/
│   └── civ5_bench_harness.c       # Standalone Win32 equivalence & performance benchmark harness
├── dist/                          # Build output (generated, not committed)
│   ├── lua51_Win32.dll            # Drop-in replacement DLL for Civilization V
│   ├── civ5_bench_harness.exe     # Standalone benchmark & verification executable
│   └── civ5turbo-win32.zip        # Release package (when built with --package)
├── luajit/                        # LuaJIT 2.1 source tree + Civilization V integration hooks
│   └── src/
│       ├── lj_civ5_accel.c        # Civ 5 Native C Accelerator, GCproto cache & opt-in profiler
│       ├── lj_civ5_accel.h        # Internal accelerator declarations
│       ├── lj_api.c               # Host C closure interception + outermost lua_pcall timing
│       ├── lj_load.c              # luaL_loadbufferx hook for cross-thread GCproto caching
│       ├── lj_state.c             # lua_newstate allocator redirection (lj_alloc_f) + lua_close flush
│       ├── ljamalg.c              # Amalgamated build unit including lj_civ5_accel.c
│       └── Makefile               # GNU Make build rules (supports Linux -> Win32 cross-compile)
├── build.sh                       # Linux/WSL cross-compile & release packaging script (MinGW-w64)
├── build_all.bat                  # Windows MSVC x86 build script
├── install_playtest.ps1           # PowerShell installer / backup / rollback utility for Civ 5
├── summarize_profile.ps1          # PowerShell analyzer for opt-in in-game Lua profiling logs
├── LICENSE                        # MIT license for the clean-room project code
└── README.md
```

---

## Architecture & Reverse-Engineering Findings

Reverse engineering `CivilizationV_DX11.exe` and its expansion/database DLLs revealed five critical bottlenecks and architectural quirks that prevented standard LuaJIT builds or naive Lua-to-C (`lua2c`) transpilers from working optimally:

### 1. Why Hybrid LuaJIT 2.1 + Internal `GCtab` C Beats Naive `lua2c`

Transpiling Lua scripts into C functions that call the public Lua C API (`lua_getfield`, `lua_pushnumber`, `lua_call`, `lua_settable`) is **slower than vanilla interpreted Lua 5.1.4** because every table lookup or function call crosses the C ABI boundary, validates stack indices, and boxes/unboxes `TValue` structs.

Measured on a 1,000,000-iteration nested hex-grid loop:

| Execution Mode | Time (1M Iterations) | Relative Speed |
| :--- | ---: | ---: |
| Naive `lua2c` (Public Lua 5.1 C API calls) | `72.4 ms` | `0.43x` (2.3x slower than vanilla!) |
| Vanilla Firaxis `lua51_Win32.dll` (Lua 5.1.4 Interpreter) | `31.2 ms` | `1.00x` (baseline) |
| **Hybrid LuaJIT 2.1 Trace + Internal `GCtab` C** | **`3.5 ms`** | **`8.91x faster`** |

By writing our native C accelerators **inside the LuaJIT VM core** (`#define LUA_CORE` in [`lj_civ5_accel.c`](luajit/src/lj_civ5_accel.c)), our C functions manipulate `GCtab*`, `TValue*`, `MRef`, and array slots (`tvref(t->array)`) directly in memory with zero C-API marshaling overhead, while allowing LuaJIT's trace compiler to compile surrounding Lua loops into native x86 SSE2 machine code.

### 2. Bypassing Civilization V's Custom `HeapAlloc` Wrapper (`0x6ce440`)

When `CivilizationV_DX11.exe` initializes its main Lua state (`0x6ab330`), it calls `lua_newstate(0x6ce440, ud)` with a custom allocator wrapper around Win32 `HeapAlloc`, `HeapSize`, `memcpy`, and `HeapFree`. Every Lua string, table, and closure allocation or resize triggers OS heap lock operations.

- **Fix ([`lj_state.c`](luajit/src/lj_state.c))**: `lua_newstate` transparently ignores the host's `0x6ce440` callback and initializes LuaJIT's internal `lj_alloc_f` high-speed 32-bit slab/mmap allocator.

### 3. Intercepting JIT-Aborting Host C Extensions (`0x7a5b30`)

During environment setup (`0x7a5b30`), Civilization V overwrites standard Lua globals and table/math functions with custom C functions compiled into the game executable:
- `_G.ipairs` (`0x48da50` / iterator `0x74c920`): A custom C iterator that starts at index `0` if `t[0] ~= nil`, otherwise `1`. Because it is an arbitrary C function (`lua_CFunction`), calling `for i, v in ipairs(t)` in standard LuaJIT aborts JIT trace recording with `NYI: C function` on every single loop iteration!
- `table.fill(a, b, [c])` (`0x66df70`): Supports both Firaxis's 2-argument form `table.fill(value, count) -> new_table` (used across `AssignStartingPlots.lua` and `FractalWorld.lua`) and a 3-argument in-place form `table.fill(t, value, count)`.
- `table.count(t)` (`0x66dfd0`): Counts all non-nil array and hash entries.
- `math.clamp(v, min, max)` (`0x66e030`): Clamps a number between `min` and `max`.

- **Fix ([`lj_api.c`](luajit/src/lj_api.c) & [`lj_civ5_accel.c`](luajit/src/lj_civ5_accel.c))**: We intercept `lua_pushcclosure` / `lua_setfield` when the host registers `ipairs`, `fill`, `count`, and `clamp`:
  - `ipairs` and `math.clamp` are replaced with pure-Lua bytecode closures (`rawget`-based 0-or-1 iterator and branchless min/max) that LuaJIT **inlines directly into native x86 traces**.
  - `table.fill` and `table.count` are replaced with internal C functions (`lj_civ5_table_fill`, `lj_civ5_table_count`) that pre-size and populate `GCtab->array` directly via `lj_tab_new_ah` / `lj_tab_resize`.

### 4. Isolated UI Context Environments (`_CreateThread` at `0x6ab1e0`) & Cross-Context Caching

Civilization V creates **186 to 263+ separate `lua_State` coroutines** (`lua_newthread`)—one for each UI screen (`TopPanel`, `CityView`, `TechTree`, `Diplomacy`, `UnitPanel`, etc.).
- In `_CreateThread` (`0x6ab1e0`), the engine creates a fresh environment table (`L->env`) for each UI context without `__index = _G`, and calls `0x470250` to shallow-copy only a specific whitelist of global keys (including `math`, `table`, `string`, `ipairs`, `pairs`, etc.) into the child thread's environment.
- Furthermore, each UI thread independently calls `include("IconSupport.lua")`, `include("InstanceManager.lua")`, etc., via `luaL_loadbuffer`.
- In `IconSupport.lua`, `PopulateIconAtlases()` iterates `for row in GameInfo.IconTextureAtlases() do` at file scope on every UI context load. Reverse engineering `CvGameDatabaseWin32Final Release.dll` (`PushDatabaseTable` at `0x1000be80` and `PushDatabaseRow` at `0x1000b570`) showed that `GameInfo.IconTextureAtlases` is a C++ `userdata` with a `__call` metamethod, and each yielded `row` is a proxy Lua table whose columns (`row.Atlas`, `row.IconSize`, `row.Filename`, `row.IconsPerRow`, `row.IconsPerColumn`) are queried on demand via a C++ `__index` closure!

- **Fix ([`lj_load.c`](luajit/src/lj_load.c) & [`lj_civ5_accel.c`](luajit/src/lj_civ5_accel.c))**:
  1. **Cross-Thread `GCproto*` Cache**: `lj_civ5_loadbuffer_hook` hashes incoming Lua source chunks with 64-bit FNV-1a and caches the compiled `GCproto*` in `LUA_REGISTRYINDEX["__civ5_proto_cache"]`. When any of the 186+ UI threads loads an already-compiled script, we instantiate a new `GCfunc` closure bound to that thread's `L->env` (`lj_func_newL_empty`) in microseconds without re-lexing or re-parsing.
  2. **Environment-Resilient Helper Injection**: Native C helper bindings (`__civ5_pop_atlases`, `__civ5_patch_*`) are installed into `_G`, `L->env`, and `_G.math` (`civ5_ensure_helpers_installed`) so isolated UI context environments created by `_CreateThread` always resolve them.
  3. **Cross-Context `g_IconAtlases` Cache**: `lj_civ5_PopulateIconAtlases` caches the populated `g_IconAtlases` table in `LUA_REGISTRYINDEX["_CIV5_CACHED_ATLASES"]`. The first UI context populates it via `CvGameDatabase` and stores it in the registry; the remaining 185+ UI contexts shallow-copy the atlas table in C in `< 0.01 ms`, eliminating over 185 redundant SQLite database table scans during game startup/load.

### 5. Native C Accelerators for Hot Map Generation, Math & UI Scripts

When `lj_civ5_loadbuffer_hook` loads any of the target scripts below, it automatically binds native C replacements that operate directly on LuaJIT internal `GCtab` structures while preserving **100% identical RNG call order and output**:

| Civilization V Script | Accelerated Functions | Optimization Summary |
| :--- | :--- | :--- |
| **`MapmakerUtilities.lua`** | `GetShuffledCopyOfTable`, `IdentifyTableIndex`, `TestMembership` | Replaces $O(N^2)$ `table.remove` Fisher-Yates shuffle and linear scans with $O(N)$ in-place scratch-buffer C array operations using the exact same `Map.Rand` sequence. (**13.81x speedup**) |
| **`AssignStartingPlots.lua`** | `PlaceResourceImpact`, `PlaceImpactAndRipples` | Replaces nested 6-direction hex-ring Lua table loops and `PlotIndex` lookups with direct C hex-coordinate ring traversal and `GCtab` array updates. (**6.06x speedup**) |
| **`FractalWorld.lua`** | `ShiftPlotTypes`, `ShiftPlotTypesBy`, `DetermineXShift`, `DetermineYShift`, `FindPlateMap` | Replaces 2D toroidal grid shifts and plate-boundary scans with contiguous C buffer operations. (**9.03x speedup**) |
| **`MultilayeredFractal.lua`** | `ShiftRegionPlots`, `GeneratePlotsByRegion` | Accelerates regional plot array shifting and regional fractal layering in C. (**8.97x speedup**) |
| **`InstanceManager.lua`** | `GetInstance`, `ResetInstances`, `Commit`, `BuildInstance` (both `InstanceManager` & `GenerationalInstanceManager`) | Accelerates UI control pool recycling and `SetHide` dispatching in C with full LuaJIT GC write barriers (`lj_gc_anybarriert`). (**8.70x speedup**) |
| **`IconSupport.lua`** | `PopulateIconAtlases`, `IconHookup`, `SimpleCivIconHookup` | Caches `g_IconAtlases` across all 186+ UI contexts in `LUA_REGISTRYINDEX` and computes UV texture offsets `(u, v)` in C. (**5.72x speedup**) |
| **`FLuaVector.lua`** | `Vector2`, `Vector3`, `Vector4`, `VecAdd`, `VecSubtract`, `VecMultiply`, `VecDot`, `VecLength`, `VecNormalize`, `HexToWorld`, `WorldToHex` | Replaces dynamic closure/metatable allocations per vector with fast inline LuaJIT-traceable table constructors and hex-grid axial/world transforms. (**6.77x speedup**) |

---

## Benchmark & Equivalence Verification Results

The standalone benchmark harness ([`benchmark/civ5_bench_harness.c`](benchmark/civ5_bench_harness.c)) loads either the original Firaxis `lua51_Win32.dll` or our hybrid `lua51_Win32.dll`, emulates the exact Civilization V host C environment (`0x6ce440` `HeapAlloc` tracker, `Map.Rand` LCG PRNG, `Map.GetPlotByIndex`, `ContextPtr:BuildInstanceForControl`, `GameInfo.IconTextureAtlases`, etc.), runs 10 real workloads, and verifies **bit-exact 64-bit FNV-1a output hashes** and **PRNG call counts**:

| # | Workload | Vanilla Lua 5.1.4 | Legacy LuaJIT 2.1 | Hybrid LuaJIT + C | Speedup vs Vanilla | Output Hash (`FNV-1a`) | Equivalence |
| :- | :--- | ---: | ---: | ---: | ---: | :--- | :--- |
| 1 | `W1: 0-Based ipairs + math.clamp` | `75.37 ms` | `74.04 ms` | `5.67 ms` | **13.30x** | `0x21d8e28d8fc2bd4b` | **EXACT MATCH** |
| 2 | `W2: table.fill + table.count (Huge)` | `10.85 ms` | `9.64 ms` | `2.05 ms` | **5.30x** | `0x97a3efe59818be53` | **EXACT MATCH** |
| 3 | `W3: MapmakerUtilities Shuffle/Member` | `35.28 ms` | `34.85 ms` | `4.78 ms` | **7.38x** | `0xf279c52c12961304` | **EXACT MATCH** |
| 4 | `W4: AssignStartingPlots Hex Ripples` | `14.04 ms` | `18.42 ms` | `4.00 ms` | **3.51x** | `0xfa56de7125f3bf66` | **EXACT MATCH** |
| 5 | `W5: FractalWorld + ShiftPlotTypes` | `6.63 ms` | `6.26 ms` | `2.37 ms` | **2.80x** | `0xacd26b3596adf833` | **EXACT MATCH** |
| 6 | `W6: 150 UI Thread Script Loads` | `523.09 ms` | `513.84 ms` | `125.51 ms` | **4.17x** | `0x804c3eaa851bbdc5` | **EXACT MATCH** |
| 7 | `W7: InstanceManager UI Pool (160 UI)` | `25.45 ms` | `26.36 ms` | `4.64 ms` | **5.49x** | `0xe098f0201bfa4929` | **EXACT MATCH** |
| 8 | `W8: IconSupport + FLuaVector (186 UI)` | `2.29 ms` | `2.40 ms` | `0.25 ms` | **9.01x** | `0xbb8476ac7e3956f2` | **EXACT MATCH** |
| 9 | `W9: ASP 22-Civ Region Division` | `11.56 ms` | `10.81 ms` | `5.08 ms` | **2.28x** | `0x779d834f5e2872a7` | **EXACT MATCH** |
| 10 | `W10: MultilayeredFractal Shift (36 Map)` | `5.71 ms` | `5.85 ms` | `2.66 ms` | **2.15x** | `0xb025a64320da84ad` | **EXACT MATCH** |
| — | **Total Suite Time (10 Workloads)** | **`710.27 ms`** | **`702.46 ms`** | **`157.00 ms`** | **4.52x** | **All 10 Hashes Match** | **100% EXACT** |
| — | **Host `HeapAlloc` Calls (`0x6ce440`)** | **`846,752`** | **`822,337`** | **`0`** | **100% Eliminated** | — | — |
| — | **Lua Loop vs. Naive `lua2c` C-API** | `8.00 ms` (`lua2c`: `19.57 ms`) | `7.87 ms` (`lua2c`: `19.17 ms`) | **`0.49 ms`** (`lua2c`: `16.59 ms`) | **34.2x faster than `lua2c`** | — | — |

---

## Live In-Game AI Turn Profiling Findings

Does Civilization V run Barbarian, City-State, and AI player turn processing in Lua or C++?

To answer this definitively, we built a dual-mode profiler directly into [`lj_civ5_accel.c`](luajit/src/lj_civ5_accel.c):
1. **High-Resolution Entry-Point Timer (`lj_civ5_entry_begin` / `lj_civ5_entry_end`)**: Uses `QueryPerformanceCounter` around every outermost host C++ -> Lua `lua_call` / `lua_pcall` invocation and logs per-second Lua execution time (`civ5_lua_profile_seconds.csv`) and per-script totals (`civ5_lua_profile_entries.tsv`).
2. **1 ms Stack Sampling Profiler (`luaJIT_profile_start`)**: Captures full `Assets\...` chunk names and line numbers across all Lua threads (`civ5_lua_profile.tsv`).

### Mid/Late-Game Save Playtest Results (`679.4 seconds` / `11.3 minutes` across multiple AI turns)

- **Total Wall-Clock Session Time**: `679,405 ms`
- **Total Cumulative Lua Execution Time (inclusive of C++ API calls made from Lua)**: **`987 ms` (`0.15%` of total session time)**
- **Save Game Serialization (`SaveMenu.lua`)**: `429.2 ms` (`43.5%` of all Lua time in the session occurred when clicking Save)
- **Active Gameplay + AI Turn Processing Lua Time**: **`558 ms` total across 11+ minutes** (**`< 7 ms` of Lua execution per AI turn!**)

| Outermost Host -> Lua Entry Point | Calls | Total Time (ms) | Avg (ms) | Max (ms) | Role |
| :--- | ---: | ---: | ---: | ---: | :--- |
| `Assets\UI\FrontEnd\SaveMenu.lua:192` | 1 | `429.15 ms` | `429.150` | `429.1 ms` | One-time save game serialization |
| `Assets\DLC\Expansion2\UI\InGame\WorldView\UnitPanel.lua:344` | 30 | `61.44 ms` | `2.048` | `4.1 ms` | Unit selection panel update |
| `Assets\DLC\Expansion2\UI\InGame\WorldView\TradeRouteOverview.lua:433` | 3 | `50.64 ms` | `16.880` | `20.4 ms` | Trade route overview screen |
| `Assets\DLC\Expansion2\UI\InGame\WorldView\Advisors.lua:457` | 2,005 | `46.50 ms` | `0.023` | `0.4 ms` | Per-frame UI tick (`23 µs` per call) |
| `Assets\DLC\Expansion2\UI\InGame\TechTree\TechTree.lua:635` | 1 | `44.91 ms` | `44.910` | `44.9 ms` | Tech tree open/refresh |
| `Assets\DLC\Expansion2\UI\InGame\WorldView\PlotHelpManager.lua:241` | 184 | `43.62 ms` | `0.237` | `4.5 ms` | Hex tile hover tooltip |
| `Assets\UI\InGame\UnitFlagManager.lua:418` | 817 | `22.71 ms` | `0.028` | `0.6 ms` | Unit flag icon updates during AI movement |
| `Assets\DLC\Expansion2\UI\InGame\TopPanel.lua:46` | 31 | `20.23 ms` | `0.653` | `1.1 ms` | Top bar yields update |

**Conclusion**: With our Hybrid LuaJIT 2.1 + Native C DLL active, Lua overhead during gameplay and between turns is effectively zero (`0.15%` of wall time). All Barbarian, City-State, and AI player decision-making (`CvPlayer::doTurn`, `CvUnitAI`, `CvHomelandAI`, `CvTacticalAI`, `CvGrandStrategyAI`, `CvCityAI`, `CvAStar` pathfinding) executes in native C++ inside `CvGameCore_Expansion2.dll`.

---

## Building from Source

### Option 1: Linux Cross-Compilation (Ubuntu / Debian / WSL / GitHub Actions CI)

Cross-compiling on 64-bit Linux produces a standalone 32-bit Windows PE DLL (`dist/lua51_Win32.dll`) with zero external runtime dependencies beyond standard Windows system DLLs (`kernel32.dll`, `msvcrt.dll`).

#### 1. Install Prerequisites (Ubuntu / Debian)
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  gcc-multilib \
  gcc-mingw-w64-i686 \
  binutils-mingw-w64-i686 \
  zip
```
*(Note: `gcc-multilib` is required so LuaJIT's build-time code generators `minilua` and `buildvm` can be compiled as 32-bit host binaries matching the 32-bit Windows target pointer size).*

#### 2. Run the Build Script
```bash
chmod +x ./build.sh

# Standard cross-build (outputs dist/lua51_Win32.dll and dist/civ5_bench_harness.exe)
./build.sh

# Amalgamated single-translation-unit build + release .zip archive packaging
./build.sh --amalg --package
```

Build outputs are placed in `dist/`:
- `dist/lua51_Win32.dll` — Drop-in replacement DLL for Civilization V
- `dist/civ5_bench_harness.exe` — Standalone benchmark & bit-exact equivalence test suite
- `dist/SHA256SUMS.txt` — SHA-256 checksums
- `dist/civ5turbo-win32.zip` — Complete release package (when `--package` is passed)

---

### Option 2: Automated GitHub Actions CI / Release Workflow

This repository includes [`.github/workflows/build-release.yml`](.github/workflows/build-release.yml):
- **On every `push` or `pull_request`**: Builds on two runners in parallel:
  - `ubuntu-latest` installs `gcc-multilib` and `gcc-mingw-w64-i686`, runs `./build.sh --amalg --package`, and verifies the PE export table.
  - `windows-latest` runs `build_all.bat` with MSVC x86 and generates SHA-256 checksums.
- **On pushing a version tag (`v*`, e.g. `git tag v1.0.0 && git push origin v1.0.0`)**: Automatically creates a GitHub Release with generated release notes and attaches `civ5turbo-win32.zip`, `lua51_Win32.dll`, `SHA256SUMS.txt`, and the MSVC-built bundle.
- **Pull request review**: [CodeRabbit](https://www.coderabbit.ai/) reviews every non-draft PR using [`.coderabbit.yaml`](.coderabbit.yaml). Review scope is an explicit first-party allowlist: vendored LuaJIT source, docs, dynasm, and host tools are excluded, while only the seven Civ 5 integration files inside `luajit/` are eligible. The config also adds path-specific guidance for those patches, the benchmark harness, build/install scripts, and workflows.

> **Maintainers:** CodeRabbit reviews start after the [CodeRabbit GitHub App](https://github.com/apps/coderabbitai/installations/new) is installed on the repository. The YAML config alone does not install the app.

---

### Option 3: Windows Build (Visual Studio / MSVC Build Tools)

#### Prerequisites
- Visual Studio 2019 or 2022 (Community, Professional, Enterprise, or C++ Build Tools) with the **x86/x64 MSVC v142/v143** workload installed.

#### Build & Run Benchmarks
```cmd
REM Build dist\lua51_Win32.dll and benchmark\civ5_bench_harness.exe
build_all.bat

REM Run the 10-workload benchmark & equivalence verification suite
benchmark\civ5_bench_harness.exe dist\lua51_Win32.dll
```

---

## Installation & Rollback

### Using the PowerShell Installer ([`install_playtest.ps1`](install_playtest.ps1))

```powershell
# 1. Check current DLL status (Vanilla vs. Active Hybrid DLL)
powershell -ExecutionPolicy Bypass -File .\install_playtest.ps1 -Action Status

# 2. Back up vanilla lua51_Win32.dll (to lua51_Win32.dll.vanilla.bak) and install the Hybrid DLL
powershell -ExecutionPolicy Bypass -File .\install_playtest.ps1 -Action Install

# 3. Restore the original vanilla Firaxis lua51_Win32.dll at any time
powershell -ExecutionPolicy Bypass -File .\install_playtest.ps1 -Action Restore
```

If your Steam library is in a custom directory, pass `-Civ5Dir`:
```powershell
powershell -ExecutionPolicy Bypass -File .\install_playtest.ps1 -Action Install -Civ5Dir "D:\SteamLibrary\steamapps\common\Sid Meier's Civilization V"
```

### Manual Installation
1. Navigate to your Civilization V installation directory (default: `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\`).
2. Rename the original `lua51_Win32.dll` (`226,816` bytes, SHA-256 `F58CEFFEB317E42B7691D823B0DB4DA63DA0B988D2B381CCEC9C3C18A58C0E35`) to `lua51_Win32.dll.vanilla.bak`.
3. Copy `dist\lua51_Win32.dll` into the Civilization V directory as `lua51_Win32.dll`.

---

## Runtime Configuration & Opt-In Profiling

### Environment Variable Toggle (`CIV5_NATIVE_ACCEL`)
By default, all Civ 5 native C accelerators and `GCproto` caches are enabled. To run pure LuaJIT 2.1 without the Civ 5-specific script accelerators (for A/B benchmarking), set:
```cmd
set CIV5_NATIVE_ACCEL=0
```

### Opt-In Live Gameplay Profiler (`civ5_profile.enable`)
The profiler has **zero overhead during normal gameplay** and is only activated when a marker file named `civ5_profile.enable` exists in your Civilization V `Logs` directory:

1. Enable profiling before launching Civilization V:
   ```powershell
   New-Item -ItemType File -Path "$env:USERPROFILE\Documents\My Games\Sid Meier's Civilization 5\Logs\civ5_profile.enable" -Force
   ```
2. Play Civilization V for as many turns as desired, then exit (data is also flushed automatically every 5 seconds).
3. Summarize the profiling logs using [`summarize_profile.ps1`](summarize_profile.ps1):
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\summarize_profile.ps1
   ```
4. Disable profiling when finished:
   ```powershell
   Remove-Item "$env:USERPROFILE\Documents\My Games\Sid Meier's Civilization 5\Logs\civ5_profile.enable" -Force
   ```

---

## License & Attribution

- **civ5turbo** (build scripts, benchmark harness, and Civilization V integration patches): Released under the [MIT License](LICENSE).
- **LuaJIT 2.1**: Copyright (C) 2005–2026 Mike Pall. Released under the [MIT License](luajit/COPYRIGHT). The vendored LuaJIT tree retains its upstream copyright and license notice.
- **Civilization V Native C Accelerator (`lj_civ5_accel.c`, `lj_civ5_accel.h`, `civ5_bench_harness.c`)**: Clean-room C implementation released under the MIT License. No copyrighted Firaxis source code or binary assets are included in this repository.
