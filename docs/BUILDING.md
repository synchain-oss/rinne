# Building Rinne from source

## Summary

Rinne is two JUCE 8 plugins (Rinne Input and Rinne Output) plus a JUCE-free core library. You need a C++17 toolchain, CMake 3.22 or newer, Ninja, a checkout of JUCE at one exact commit, and, on Windows, internet access at configure time to fetch the pinned WebView2 SDK package. This page is enough to build from a clean machine. The only plugin formats built are VST3 (Windows and macOS) and Audio Unit (macOS).

## What gets pinned

Pins live in files at the repository root. Read the file, not this page, when they disagree.

| File | What it pins |
|---|---|
| `.juce-version`, `.juce-commit` | The JUCE release tag and the exact commit your JUCE checkout must be at |
| `.node-version` | The Node version for the repository scripts (checks, doc generators) |
| `.pluginval-version` | The pluginval release used to validate the plugins |
| `.gitleaks-version`, `.clang-format-version` | The secret scanner and formatter used by the checks |

The WebView2 SDK version is `WEBVIEW2_VERSION` in `cmake/RinneDeps.cmake`.

## JUCE

JUCE is never downloaded by the build. Check it out yourself, move it to the pinned commit, and pass its path in with `-DJUCE_PATH`.

```sh
git clone https://github.com/juce-framework/JUCE.git
git -C JUCE checkout "$(cat .juce-commit)"
```

CMake refuses to configure if the checkout is not at the commit in `.juce-commit`. The licence terms for the binary, JUCE included, are in the licence section of the [README](../README.md).

## Windows (x64, VST3)

You need Visual Studio 2022 with the C++ desktop workload (the build uses the static runtime, so the plugins do not need the Visual C++ redistributable), CMake, Ninja, and PowerShell 7.

```powershell
pwsh scripts/build.ps1 -JucePath <path to the JUCE checkout>
```

The script configures Ninja Multi-Config in `build/`, builds the Release configuration, and checks for first-party compiler warnings (there must be none). Useful switches:

- `-Test` runs the tests with ctest after the build.
- `-Validate` runs pluginval at strictness 5, without a GUI, on both plugins.
- `-Config Debug` builds another configuration.

You can also call CMake directly:

```powershell
cmake -S . -B build -G "Ninja Multi-Config" -DJUCE_PATH=<path to the JUCE checkout>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The WebView2 SDK package is downloaded from nuget.org during configure and checked against a pinned SHA-256. At run time the plugins need the Microsoft Edge WebView2 Runtime on the machine.

## macOS (Apple Silicon, VST3 and Audio Unit)

You need Xcode with its command line tools, CMake, and Ninja. The build targets arm64 with macOS 11.0 as the deployment target.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH=<path to the JUCE checkout>
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
```

The result is two `.vst3` bundles and two `.component` bundles. They are not signed with a Developer ID and not notarized.

## Without JUCE: the core library and its tests

`rinne_core` does not include or link JUCE. You can build it and its tests alone, which is also how the sanitizer builds run:

```sh
cmake -S . -B build-core -G Ninja -DRINNE_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure --no-tests=error
```

Add `-DRINNE_SANITIZE=asan_ubsan` or `-DRINNE_SANITIZE=tsan` for the sanitizer configurations.

## Options

| Option | Default | Meaning |
|---|---|---|
| `RINNE_BUILD_TESTS` | ON | Build the tests |
| `RINNE_BUILD_TOOLS` | ON | Build the helper tools under `tools/` |
| `RINNE_CORE_ONLY` | OFF | Build only `rinne_core` and its tests; needs no JUCE |
| `RINNE_SANITIZE` | empty | `asan_ubsan` or `tsan`; applies to the core and its tests |
| `RINNE_PROBE_BUILD` | OFF | Build with a separate probe identity (plugin codes, bundle ids, shared-memory prefix) |
| `RINNE_BUILD_LABEL` | empty | A label written into the build information; letters, digits and `. + -` only |

## Pairing between builds

The two plugins find each other through a shared-memory name that is fixed at build time. A plain CMake configure uses the release name. `scripts/build.ps1` uses a different name on a local build, so that a build from source never pairs with an installed release. Either way, both plugins must come from the same build: a Rinne Input from one build and a Rinne Output from another will not find each other.

## Before you open a pull request

Run the local gates and the static checks (see [CONTRIBUTING](../CONTRIBUTING.md)):

```powershell
pwsh scripts/gates.ps1
node scripts/run-checks.mjs
```

The scripts use the Node version in `.node-version`, not whatever is installed globally. Documentation that comes from a generator (marked `BEGIN gen:` blocks) is regenerated with `node scripts/gen-docs.mjs --write`, never edited by hand.
