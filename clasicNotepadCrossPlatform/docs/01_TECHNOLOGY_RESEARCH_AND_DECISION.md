# Classic Notepad Cross-Platform: Technology Research and Decision

Date: 2026-04-20  
Scope: macOS-first implementation, Linux desktop support (Ubuntu default desktop install), and buildability on macOS/Linux/Windows.

## 1) Constraints and Goals

- Keep the app native on each OS (no browser shell, no Electron/WebView framework).
- Use free tooling.
- Minimize non-OS dependencies.
- Match Classic Notepad behavior. Dark mode was excluded from the initial 2026-04-20 scope, then added by the 2026-04-26 spelling/dark-mode follow-up.
- Start development on macOS, then add Linux parity.

## 2) Candidate Approaches

## Option A — C++ core + native platform UI (AppKit on macOS, GTK on Linux)

**What it means**
- Shared C++ core for document model, encoding, line endings, find/replace logic.
- macOS UI implemented in Objective-C++ (`.mm`) using AppKit (`NSTextView`, `NSMenu`, `NSOpenPanel`, `NSSavePanel`, print and page setup).
- Linux UI implemented in C/C++ using GTK (`GtkApplication`, `GtkTextView`, `GtkFileDialog` or compatible chooser APIs).

**Pros**
- Most direct “native” fit for both target desktops.
- Mature system APIs and docs.
- Keeps runtime small.
- CMake works on macOS/Linux/Windows.

**Cons**
- Two UI implementations.
- Objective-C++ + C/C++ mix requires careful boundaries.

## Option B — Rust core + native platform UI through FFI

**What it means**
- Rust core for text/document services.
- Platform UIs still need AppKit + GTK bridges.

**Pros**
- Strong memory-safety model.
- Rust has Tier 1 support for major macOS/Linux/Windows targets.

**Cons**
- Native UI bindings usually introduce external crates and ecosystem dependencies.
- No practical “pure OS-only” native GUI path without significant FFI complexity.
- Higher integration overhead for a first version.

## Option C — Single cross-platform widget toolkit (Qt/GTK/wx/etc.)

**Pros**
- One UI codebase.

**Cons**
- Contradicts “as native as possible” and “no extra libraries beyond OS-provided” intent.
- Packaging/licensing/runtime details become the main work.

## 3) Decision

**Decision: Option A (C++ core + native UI per platform).**

Reasoning:
1. Best match to “native feel” on both target OSes.
2. Keeps dependency policy tight.
3. Lowest long-term risk for behavior parity with classic Notepad patterns.
4. Leaves room to add a Rust core later if needed, without changing platform UI architecture.

## 4) Free Build Toolchain (Documented)

### macOS
- Xcode Command Line Tools (clang/clang++, SDKs, codesign utilities).
- CMake.
- Ninja or Xcode generator.

### Ubuntu Desktop
- `build-essential` (gcc/g++, make).
- `cmake`.
- `ninja-build` (optional but recommended).
- `pkg-config`.
- GTK dev package for the chosen GTK major line.

### Windows (buildability requirement)
- Visual Studio Build Tools (free) or Visual Studio Community.
- CMake.
- Optional MinGW toolchain for non-MSVC experiments.

## 5) Spell Checking Feasibility (No third-party app libs)

- **macOS:** Use `NSSpellChecker`/`NSTextView` capabilities directly.
- **Linux:** No single guaranteed desktop-wide spell API baseline like macOS AppKit’s spell services; spell usually comes via libraries/toolkits (for example Enchant-backed stacks). For v1, keep spell check optional/deferred behind a feature flag and do not block editor release on it.

## 6) Source References

- Apple AppKit spell APIs (`NSSpellChecker`): https://developer.apple.com/documentation/appkit/nsspellchecker
- Apple TextKit overview: https://developer.apple.com/documentation/appkit/textkit
- GTK4 `GtkTextView` reference: https://docs.gtk.org/gtk4/class.TextView.html
- GTK API index: https://docs.gtk.org/gtk4/
- Rust platform support tiers: https://doc.rust-lang.org/rustc/platform-support.html
- CMake cross-platform build system overview: https://cmake.org/about/ and https://cmake.org/features/
- Ubuntu package index showing Enchant development packages (evidence of Linux spell stack packaging reality): https://packages.ubuntu.com/search?keywords=enchant

## 7) Decision Tree (concise)

1. Need native look/feel on macOS + Ubuntu?  
   - Yes → Prefer per-platform native UI.
2. Need minimal non-OS dependencies?  
   - Yes → Avoid heavyweight cross-platform GUI frameworks.
3. Need fastest path from macOS-first to Linux parity?  
   - Yes → Shared C++ core + thin platform adapters.
4. Result:  
   - **C++ shared core + AppKit adapter + GTK adapter**.
