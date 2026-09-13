# Burlak — rules for every agent working in this repository

Read `docs/ARCHITECTURE.md` before touching anything. That file is the design; this file is the
law. If the two disagree, stop and report the conflict instead of picking one.

## What we are building

A Far Manager 3 plugin (`Burlak.dll`, x64 / x86 / arm64) that drags files out of Far's panels
into any Windows drop target, and — in the work ahead — from plugin panels, into the other panel
of the same Far, and into another Far window. Far's plugin headers are vendored verbatim under
`sdk/` (never edit them). Far's own sources are the reference for how it treats mouse input and
plugin panels; when a behaviour of Far matters, cite the file and function you read.

## Non-negotiable rules

1. **Language.** C++23 as the dialect (`/std:c++latest` under MSVC, `-std=c++23` under clang-cl),
   using only what both compilers accept; the release DLLs are built with MSVC, the tests and the
   coverage build with clang-cl 19. No C translation units of our own.
2. **Ownership.** `std::unique_ptr` only. Forbidden under `src/`: `new`, `delete`, `malloc`,
   `free`, `shared_ptr`, `weak_ptr`, owning raw pointers, manually managed arrays. COM interface
   pointers are held in an RAII wrapper (`Microsoft::WRL::ComPtr` or a small local one). Win32
   handles are held in RAII wrappers.
3. **Non-owning.** `std::span` for ranges, references for object dependencies,
   `std::string_view`/`std::wstring_view` for input strings. A raw pointer appears only on the
   line inside an adapter that calls the foreign API, and in `src/plugin/Exports.cpp`, which
   speaks the C ABI and contains no logic.
4. **Layers.** Foreign APIs (Win32, COM/shell, the Far API, other plugins' exports) are touched
   only under `src/adapters/**`, `src/drag/**` and `src/plugin/**`. `src/core/**` includes no
   foreign header and takes no foreign type. Adapters are one-to-one over the foreign API and
   contain no decisions; decisions live in core. The guard test enforces the include rule.
   The guard test under `tests/guard/` is a lexical safety net for rules 2-4: it catches the
   obvious shapes and says in its header what it cannot see. It is not the proof; the reviewer
   proves ownership and layering by reading the code.
5. **Errors.** Expected failures (no selection, panel without paths, the host window not found,
   a plugin refusing to extract, a peer gone) are `std::expected<T, Error>` or `std::optional`.
   Exceptions are reserved for `std::bad_alloc` and broken invariants; the seven exports are
   wrapped in a catch-all firewall so nothing propagates into Far. The single `__try/__except`
   lives in `src/adapters/far/PluginCall.cpp`.
6. **TDD.** Failing test first, then the minimal code that passes, then refactor. Tests and the
   code they cover land in the same unit of work.
7. **Coverage.** 100 % lines AND 100 % branches on `src/**`, measured with llvm-cov through
   `scripts/coverage.ps1`. Not 99.9. No `LCOV_EXCL`-style markers, no uninstrumented files, no
   `__builtin_unreachable`, `[[assume]]` or `#ifdef` tricks to hide a branch: restructure instead.
8. **Warnings.** `/W4 /WX` under both compilers. Silencing a warning needs a one-line written
   reason next to the pragma or flag.
9. **Reuse before custom infrastructure.** doctest for tests (vendored single header under
   `third_party/doctest/`, unmodified), the standard library for everything else. Do not write a
   test framework, a logging framework, a JSON library or a command-line parser.
10. **No commits.** The orchestrator commits. Never run `git commit`, `git push`, `git reset`,
    `git checkout -- <file>`, `git clean`, `git stash` or anything else that rewrites the working
    tree or history. Staging (`git add`) is fine. No git worktrees; work in this checkout.
11. **Stay inside the repo.** Write only under this repository, `build/`, and `%TEMP%`. Never
    touch `C:\Tools\FarManager`.
12. **Comments say why, not what.** The existing source is the style reference: a comment names
    the reason a line exists or the Far/Windows behaviour it answers to, and cites the source
    where that came from. No narration of the code.
13. **Report honestly.** Finish with: what was done, what was not, the exact commands run and
    their results (test counts, coverage numbers, lint output, exports). No claim without the
    output that proves it.

## Toolchain (installed; do not install other compilers)

- MSVC 14.44 from VS 2022 Build Tools with the x64, x86 and arm64 toolsets; CMake's
  `Visual Studio 17 2022` generator finds it (`-A x64 | Win32 | ARM64`), no vcvars needed.
- clang-cl 19.1.5, lld-link, llvm-cov, llvm-profdata, clang-format, clang-tidy, llvm-rc:
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\`.
  clang-cl auto-detects the MSVC STL and the Windows SDK.
- CMake 4.4 and Ninja 1.13 on PATH (scoop). cppcheck 2.21, BinSkim 4.4 on PATH (scoop);
  PSScriptAnalyzer 1.25 installed for Windows PowerShell.
- Scripts must run under both Windows PowerShell 5.1 (`powershell -File`, what this machine has
  on PATH) and pwsh 7 (what CI uses).
- Git Bash quirk: set `MSYS_NO_PATHCONV=1` when passing `/flags` to cl or clang-cl from bash.
  Prefer the CMake presets over ad-hoc compiler invocations.
- Build directories are `build/<preset>`; never share one between concurrent agents.

## Definition of done for any task

- Tests were written first and are all green (`ctest` output in the report).
- `scripts/coverage.ps1` passes at 100 % lines / 100 % branches on `src/**` (numbers in the report).
- The guard test passes.
- All three release presets build with zero warnings; `Burlak.dll` exports exactly
  `GetGlobalInfoW SetStartupInfoW GetPluginInfoW OpenW ProcessConsoleInputW ProcessSynchroEventW
  ExitFARW`.
- `scripts/lint.ps1` reports zero findings. A new suppression carries a one-line reason in the
  configuration file it lives in or next to the `NOLINT`.
- `scripts/package.ps1` produces `Burlak-<version>-<arch>.zip` with the `Burlak\` folder holding
  `Burlak.dll`, `ChangeLog`, `readme_en.txt`, `readme_ru.txt`, `LICENSE` and nothing else.
- User-facing behaviour changes bump `src/plugin/version.h` (SemVer) and get a `dist/ChangeLog`
  entry in Russian, dated `DD.MM.YYYY`.
