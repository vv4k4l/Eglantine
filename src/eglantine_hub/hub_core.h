// SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Eglantine Hub core: everything that does not need Qt. Reads game containers (NSP/NSZ/XCI/XCZ,
// ExeFS "main" folders, NRO homebrew) without any keys, and launches the emulator with
// CPU-aware process settings. Kept free of Qt so it can be unit tested on its own.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Hub {

enum class GameFormat { NSP, NSZ, XCI, XCZ, ExeFS, NRO };
enum class ContentKind { Unknown, Base, Update, DLC };
enum class Integrity { Unknown, Ok, Truncated, Invalid };

struct GameFile {
    std::filesystem::path path; ///< The file to boot. For ExeFS games, the "main" file.
    GameFormat format = GameFormat::NSP;
    std::string name;           ///< Display name, cleaned from the file or folder name.
    std::optional<std::uint64_t> title_id;
    ContentKind kind = ContentKind::Unknown;
    std::string version;        ///< "v65536" style tag from the file name, if any.
    std::uint64_t size_bytes = 0;
    Integrity integrity = Integrity::Unknown;
    std::string integrity_detail;
};

/// Returns the game format for a path by its name, or nothing if it is not a bootable game.
std::optional<GameFormat> FormatFromPath(const std::filesystem::path& path);

/// Reads a game file's headers (never its encrypted contents) and fills in what can be known.
GameFile InspectGame(const std::filesystem::path& path, GameFormat format);

/// Finds every game under a folder. `should_cancel` is polled between files.
std::vector<GameFile> ScanFolder(const std::filesystem::path& root, bool recursive,
                                 const std::function<bool()>& should_cancel = {});

std::string CleanTitle(std::string_view file_stem);
std::optional<std::uint64_t> TitleIdFromText(std::string_view text);
ContentKind KindFromTitleId(std::uint64_t title_id);
std::uint64_t BaseTitleId(std::uint64_t title_id);
std::string FormatTitleId(std::uint64_t title_id); ///< 16 upper-case hex digits.
std::string FormatSize(std::uint64_t bytes);
std::string_view FormatName(GameFormat format);
std::string_view KindName(ContentKind kind);

// ---------------------------------------------------------------------------------------------
// Launching

struct LaunchOptions {
    bool fullscreen = false;
    /// Windows HIGH_PRIORITY_CLASS. Helps when other apps compete for the CPU.
    bool high_priority = false;
    /// On hybrid Intel CPUs (12th gen and newer), keep the emulator on the performance cores.
    bool performance_cores_only = false;
    /// Opt out of Windows EcoQoS/power throttling so the emulator is never parked on slow clocks.
    bool disable_power_throttling = true;
    std::string user;                    ///< Profile name or index passed with -u.
    std::vector<std::string> extra_args; ///< Passed through unchanged.
};

struct CpuTopology {
    int logical_processors = 0;
    std::uint64_t performance_mask = 0; ///< Logical processors of the fastest core class.
    std::uint64_t efficiency_mask = 0;  ///< Everything else.
    bool hybrid = false;
};

CpuTopology DetectCpuTopology();
int CountBits(std::uint64_t mask);

std::vector<std::string> BuildArguments(const std::filesystem::path& game,
                                        const LaunchOptions& options);
/// Quotes one argument the way CommandLineToArgvW / the MSVC runtime parse it.
std::string QuoteWindowsArgument(std::string_view argument);

struct LaunchResult {
    bool ok = false;
    std::string error;
    std::int64_t pid = 0;
};

/// Starts the emulator. On Windows the process is created suspended, tuned, then resumed, so the
/// settings apply from its very first instruction.
LaunchResult LaunchEmulator(const std::filesystem::path& emulator, const std::filesystem::path& game,
                            const LaunchOptions& options);

/// Blocks until the process exits. Returns false if it could not be waited on.
bool WaitForExit(std::int64_t pid);

} // namespace Hub
