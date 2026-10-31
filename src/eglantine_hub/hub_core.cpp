// SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hub_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sched.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace Hub {

namespace {

std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string PathToUtf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

bool IsHexDigit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

/// Little-endian reads from a file at absolute offsets.
class Reader {
public:
    explicit Reader(const fs::path& path) : stream_(path, std::ios::binary) {
        std::error_code ec;
        size_ = fs::file_size(path, ec);
        if (ec) {
            size_ = 0;
        }
    }

    bool IsOpen() const {
        return stream_.is_open() && size_ > 0;
    }

    std::uint64_t Size() const {
        return size_;
    }

    bool Read(std::uint64_t offset, void* out, std::size_t length) {
        if (offset > size_ || length > size_ - offset) {
            return false;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(static_cast<char*>(out), static_cast<std::streamsize>(length));
        return static_cast<std::size_t>(stream_.gcount()) == length;
    }

    std::optional<std::uint32_t> U32(std::uint64_t offset) {
        std::array<unsigned char, 4> b{};
        if (!Read(offset, b.data(), b.size())) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
               (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    }

    std::optional<std::uint64_t> U64(std::uint64_t offset) {
        const auto lo = U32(offset);
        const auto hi = U32(offset + 4);
        if (!lo || !hi) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*lo) | (static_cast<std::uint64_t>(*hi) << 32);
    }

    bool Magic(std::uint64_t offset, std::string_view magic) {
        std::array<char, 8> b{};
        if (magic.size() > b.size() || !Read(offset, b.data(), magic.size())) {
            return false;
        }
        return std::string_view(b.data(), magic.size()) == magic;
    }

private:
    std::ifstream stream_;
    std::uint64_t size_ = 0;
};

struct PartitionEntry {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string name;
};

struct Partition {
    std::vector<PartitionEntry> entries;
    std::uint64_t data_start = 0; ///< Absolute offset of the data area.
    std::uint64_t end = 0;        ///< Absolute offset where the last file ends.
};

/// Parses a PFS0 (NSP/NSZ) or HFS0 (XCI partitions) header at `base`.
std::optional<Partition> ReadPartition(Reader& reader, std::uint64_t base, bool hashed) {
    if (!reader.Magic(base, hashed ? "HFS0" : "PFS0")) {
        return std::nullopt;
    }
    const auto count = reader.U32(base + 4);
    const auto strings_size = reader.U32(base + 8);
    if (!count || !strings_size || *count > 100000 || *strings_size > 16 * 1024 * 1024) {
        return std::nullopt;
    }
    const std::uint64_t entry_size = hashed ? 0x40 : 0x18;
    const std::uint64_t entries_start = base + 0x10;
    const std::uint64_t strings_start = entries_start + entry_size * *count;

    std::string strings(*strings_size, '\0');
    if (*strings_size > 0 && !reader.Read(strings_start, strings.data(), strings.size())) {
        return std::nullopt;
    }

    Partition partition;
    partition.data_start = strings_start + *strings_size;
    partition.end = partition.data_start;
    partition.entries.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::uint64_t at = entries_start + entry_size * i;
        const auto offset = reader.U64(at);
        const auto size = reader.U64(at + 8);
        const auto name_offset = reader.U32(at + 16);
        if (!offset || !size || !name_offset || *name_offset >= strings.size()) {
            return std::nullopt;
        }
        PartitionEntry entry;
        entry.offset = *offset;
        entry.size = *size;
        entry.name = std::string(strings.c_str() + *name_offset);
        partition.end = std::max(partition.end, partition.data_start + *offset + *size);
        partition.entries.push_back(std::move(entry));
    }
    return partition;
}

void CheckNsp(Reader& reader, GameFile& game) {
    const auto partition = ReadPartition(reader, 0, false);
    if (!partition) {
        game.integrity = Integrity::Invalid;
        game.integrity_detail = "Not a valid NSP (no PFS0 header)";
        return;
    }
    if (partition->end > reader.Size()) {
        game.integrity = Integrity::Truncated;
        game.integrity_detail = "File is cut short: expected " + FormatSize(partition->end) +
                                ", found " + FormatSize(reader.Size()) +
                                ". The download or copy probably did not finish.";
    } else {
        game.integrity = Integrity::Ok;
        game.integrity_detail =
            "Container intact (" + std::to_string(partition->entries.size()) + " files)";
    }
    // A ticket is named after its rights ID, whose first 16 hex digits are the title ID.
    if (!game.title_id) {
        for (const auto& entry : partition->entries) {
            const std::string lower = ToLower(entry.name);
            if (lower.size() == 36 && lower.ends_with(".tik")) {
                if (const auto id = TitleIdFromText(entry.name.substr(0, 16))) {
                    game.title_id = id;
                    break;
                }
            }
        }
    }
}

void CheckXci(Reader& reader, GameFile& game) {
    // Some dumps keep the 0x1000-byte key area in front of the card header.
    std::uint64_t base = 0;
    if (!reader.Magic(0x100, "HEAD")) {
        if (reader.Magic(0x1100, "HEAD")) {
            base = 0x1000;
        } else {
            game.integrity = Integrity::Invalid;
            game.integrity_detail = "Not a valid XCI (no card header)";
            return;
        }
    }
    // The last page that holds data. Trimmed dumps end right after it, so anything shorter is
    // missing game data.
    const auto valid_end_page = reader.U32(base + 0x118);
    if (valid_end_page) {
        const std::uint64_t needed = base + static_cast<std::uint64_t>(*valid_end_page) * 0x200;
        if (reader.Size() < needed) {
            game.integrity = Integrity::Truncated;
            game.integrity_detail = "File is cut short: expected at least " + FormatSize(needed) +
                                    ", found " + FormatSize(reader.Size()) + ".";
            return;
        }
    }
    const auto root_offset = reader.U64(base + 0x130);
    if (!root_offset || !ReadPartition(reader, base + *root_offset, true)) {
        game.integrity = Integrity::Invalid;
        game.integrity_detail = "Card header found but its file table is unreadable";
        return;
    }
    game.integrity = Integrity::Ok;
    game.integrity_detail = "Card image intact";
}

void CheckExeFs(const fs::path& main_path, GameFile& game) {
    const fs::path npdm = main_path.parent_path() / "main.npdm";
    Reader reader(npdm);
    if (!reader.IsOpen() || !reader.Magic(0, "META")) {
        game.integrity = Integrity::Invalid;
        game.integrity_detail = "main.npdm is missing or invalid";
        return;
    }
    const auto aci0_offset = reader.U32(0x70);
    if (aci0_offset && reader.Magic(*aci0_offset, "ACI0")) {
        if (const auto program_id = reader.U64(*aci0_offset + 0x10); program_id && *program_id) {
            game.title_id = *program_id;
        }
    }
    game.integrity = Integrity::Ok;
    game.integrity_detail = "ExeFS folder with main.npdm";
}

std::string VersionFromText(std::string_view text) {
    // [v65536] or (v65536)
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if ((text[i] == '[' || text[i] == '(') && (text[i + 1] == 'v' || text[i + 1] == 'V')) {
            std::size_t j = i + 2;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
            if (j > i + 2 && j < text.size() && (text[j] == ']' || text[j] == ')')) {
                return "v" + std::string(text.substr(i + 2, j - i - 2));
            }
        }
    }
    return {};
}

/// ExeFS games are named after their folder; skip generic folder names like "exefs".
std::string ExeFsDisplayName(const fs::path& main_path) {
    fs::path folder = main_path.parent_path();
    for (int depth = 0; depth < 3 && !folder.empty(); ++depth) {
        const std::string lower = ToLower(PathToUtf8(folder.filename()));
        if (!lower.empty() && lower != "exefs" && lower != "code" && lower != "main") {
            return PathToUtf8(folder.filename());
        }
        folder = folder.parent_path();
    }
    return "Unnamed game";
}

} // namespace

std::optional<GameFormat> FormatFromPath(const fs::path& path) {
    const std::string name = ToLower(PathToUtf8(path.filename()));
    if (name == "main") {
        std::error_code ec;
        if (fs::is_regular_file(path.parent_path() / "main.npdm", ec)) {
            return GameFormat::ExeFS;
        }
        return std::nullopt;
    }
    const std::string ext = ToLower(PathToUtf8(path.extension()));
    if (ext == ".nsp") {
        return GameFormat::NSP;
    }
    if (ext == ".nsz") {
        return GameFormat::NSZ;
    }
    if (ext == ".xci") {
        return GameFormat::XCI;
    }
    if (ext == ".xcz") {
        return GameFormat::XCZ;
    }
    if (ext == ".nro") {
        return GameFormat::NRO;
    }
    return std::nullopt;
}

GameFile InspectGame(const fs::path& path, GameFormat format) {
    GameFile game;
    game.path = path;
    game.format = format;
    std::error_code ec;

    const std::string file_name = PathToUtf8(path.filename());
    if (format == GameFormat::ExeFS) {
        game.name = ExeFsDisplayName(path);
        for (const auto& entry : fs::directory_iterator(path.parent_path(), ec)) {
            if (entry.is_regular_file(ec)) {
                game.size_bytes += entry.file_size(ec);
            }
        }
        CheckExeFs(path, game);
    } else {
        game.name = CleanTitle(PathToUtf8(path.stem()));
        game.size_bytes = fs::file_size(path, ec);
        game.title_id = TitleIdFromText(file_name);
        game.version = VersionFromText(file_name);

        Reader reader(path);
        if (!reader.IsOpen()) {
            game.integrity = Integrity::Invalid;
            game.integrity_detail = "File is empty or unreadable";
        } else {
            switch (format) {
            case GameFormat::NSP:
            case GameFormat::NSZ:
                CheckNsp(reader, game);
                break;
            case GameFormat::XCI:
            case GameFormat::XCZ:
                CheckXci(reader, game);
                break;
            case GameFormat::NRO:
                if (reader.Magic(0x10, "NRO0")) {
                    game.integrity = Integrity::Ok;
                    game.integrity_detail = "Homebrew program";
                } else {
                    game.integrity = Integrity::Invalid;
                    game.integrity_detail = "Not a valid NRO";
                }
                break;
            case GameFormat::ExeFS:
                break;
            }
        }
    }

    if (game.format == GameFormat::NRO) {
        game.kind = ContentKind::Base;
    } else if (game.title_id) {
        game.kind = KindFromTitleId(*game.title_id);
    }
    if (game.name.empty()) {
        game.name = file_name;
    }
    return game;
}

std::vector<GameFile> ScanFolder(const fs::path& root, bool recursive,
                                 const std::function<bool()>& should_cancel) {
    std::vector<GameFile> games;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return games;
    }

    const auto visit = [&games](const fs::directory_entry& entry) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec)) {
            return;
        }
        if (const auto format = FormatFromPath(entry.path())) {
            games.push_back(InspectGame(entry.path(), *format));
        }
    };

    const auto options = fs::directory_options::skip_permission_denied;
    if (recursive) {
        fs::recursive_directory_iterator it(root, options, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (should_cancel && should_cancel()) {
                break;
            }
            // Eglantine's own data folder holds installed NAND content, not loose games.
            if (it->is_directory(ec) && ToLower(PathToUtf8(it->path().filename())) == "nand") {
                it.disable_recursion_pending();
                continue;
            }
            visit(*it);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(root, options, ec)) {
            if (should_cancel && should_cancel()) {
                break;
            }
            visit(entry);
        }
    }
    return games;
}

std::string CleanTitle(std::string_view file_stem) {
    std::string out;
    out.reserve(file_stem.size());
    int depth = 0;
    for (const char c : file_stem) {
        if (c == '[' || c == '(' || c == '{') {
            ++depth;
            out.push_back(' ');
            continue;
        }
        if ((c == ']' || c == ')' || c == '}') && depth > 0) {
            --depth;
            continue;
        }
        if (depth > 0) {
            continue;
        }
        out.push_back(c == '_' ? ' ' : c);
    }
    // Collapse whitespace and trim.
    std::string collapsed;
    collapsed.reserve(out.size());
    for (const char c : out) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (space && (collapsed.empty() || collapsed.back() == ' ')) {
            continue;
        }
        collapsed.push_back(space ? ' ' : c);
    }
    while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '-')) {
        collapsed.pop_back();
    }
    return collapsed.empty() ? std::string(file_stem) : collapsed;
}

std::optional<std::uint64_t> TitleIdFromText(std::string_view text) {
    // Title IDs are 16 hex digits starting with 01, e.g. 0100A3D008C5C000. Require a non-hex
    // character (or the edge) on both sides so longer hex runs such as NCA IDs do not match.
    for (std::size_t i = 0; i + 16 <= text.size(); ++i) {
        if (text[i] != '0' || text[i + 1] != '1') {
            continue;
        }
        if (i > 0 && IsHexDigit(text[i - 1])) {
            continue;
        }
        if (i + 16 < text.size() && IsHexDigit(text[i + 16])) {
            continue;
        }
        bool all_hex = true;
        for (std::size_t j = i; j < i + 16; ++j) {
            all_hex = all_hex && IsHexDigit(text[j]);
        }
        if (all_hex) {
            return std::stoull(std::string(text.substr(i, 16)), nullptr, 16);
        }
    }
    return std::nullopt;
}

ContentKind KindFromTitleId(std::uint64_t title_id) {
    const std::uint64_t low = title_id & 0x1FFF;
    if (low == 0) {
        return ContentKind::Base;
    }
    if (low == 0x800) {
        return ContentKind::Update;
    }
    if ((title_id & 0x1000) != 0) {
        return ContentKind::DLC;
    }
    return ContentKind::Unknown;
}

std::uint64_t BaseTitleId(std::uint64_t title_id) {
    switch (KindFromTitleId(title_id)) {
    case ContentKind::Update:
        return title_id & ~std::uint64_t{0xFFF};
    case ContentKind::DLC:
        return (title_id - 0x1000) & ~std::uint64_t{0xFFF};
    case ContentKind::Base:
    case ContentKind::Unknown:
        break;
    }
    return title_id;
}

std::string FormatTitleId(std::uint64_t title_id) {
    std::array<char, 17> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%016llX",
                  static_cast<unsigned long long>(title_id));
    return buffer.data();
}

std::string FormatSize(std::uint64_t bytes) {
    constexpr std::array<const char*, 5> units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), unit == 0 ? "%.0f %s" : "%.1f %s", value,
                  units[unit]);
    return buffer.data();
}

std::string_view FormatName(GameFormat format) {
    switch (format) {
    case GameFormat::NSP:
        return "NSP";
    case GameFormat::NSZ:
        return "NSZ";
    case GameFormat::XCI:
        return "XCI";
    case GameFormat::XCZ:
        return "XCZ";
    case GameFormat::ExeFS:
        return "ExeFS (main)";
    case GameFormat::NRO:
        return "NRO homebrew";
    }
    return "Unknown";
}

std::string_view KindName(ContentKind kind) {
    switch (kind) {
    case ContentKind::Base:
        return "Game";
    case ContentKind::Update:
        return "Update";
    case ContentKind::DLC:
        return "DLC";
    case ContentKind::Unknown:
        break;
    }
    return "Unknown";
}

int CountBits(std::uint64_t mask) {
    int count = 0;
    while (mask != 0) {
        mask &= mask - 1;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// CPU topology

#ifdef _WIN32

CpuTopology DetectCpuTopology() {
    CpuTopology topology;
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        return topology;
    }
    std::vector<unsigned char> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                    length, &length, GetCurrentProcess(), 0)) {
        return topology;
    }

    struct Cpu {
        BYTE efficiency_class;
        BYTE index;
    };
    std::vector<Cpu> cpus;
    for (ULONG offset = 0; offset < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (info->Size == 0) {
            break;
        }
        // Affinity masks only cover processor group 0 (up to 64 logical processors).
        if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 &&
            info->CpuSet.LogicalProcessorIndex < 64) {
            cpus.push_back({info->CpuSet.EfficiencyClass, info->CpuSet.LogicalProcessorIndex});
        }
        offset += info->Size;
    }
    if (cpus.empty()) {
        return topology;
    }

    BYTE fastest = 0;
    BYTE slowest = 0xFF;
    for (const Cpu& cpu : cpus) {
        fastest = std::max(fastest, cpu.efficiency_class);
        slowest = std::min(slowest, cpu.efficiency_class);
    }
    topology.logical_processors = static_cast<int>(cpus.size());
    topology.hybrid = fastest != slowest;
    for (const Cpu& cpu : cpus) {
        const std::uint64_t bit = std::uint64_t{1} << cpu.index;
        if (cpu.efficiency_class == fastest) {
            topology.performance_mask |= bit;
        } else {
            topology.efficiency_mask |= bit;
        }
    }
    return topology;
}

#else

namespace {
std::uint64_t ParseCpuList(const std::string& list) {
    // "0-15,18" style lists from sysfs.
    std::uint64_t mask = 0;
    std::size_t pos = 0;
    while (pos < list.size()) {
        std::size_t next = list.find(',', pos);
        if (next == std::string::npos) {
            next = list.size();
        }
        const std::string part = list.substr(pos, next - pos);
        const std::size_t dash = part.find('-');
        try {
            const int first = std::stoi(part.substr(0, dash));
            const int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
            for (int cpu = first; cpu <= last && cpu < 64; ++cpu) {
                mask |= std::uint64_t{1} << cpu;
            }
        } catch (...) {
            // Ignore malformed parts.
        }
        pos = next + 1;
    }
    return mask;
}

std::string ReadLine(const char* path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}
} // namespace

CpuTopology DetectCpuTopology() {
    CpuTopology topology;
    topology.logical_processors = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    const std::uint64_t core = ParseCpuList(ReadLine("/sys/devices/cpu_core/cpus"));
    const std::uint64_t atom = ParseCpuList(ReadLine("/sys/devices/cpu_atom/cpus"));
    if (core != 0 && atom != 0) {
        topology.hybrid = true;
        topology.performance_mask = core;
        topology.efficiency_mask = atom;
    } else {
        const int n = std::min(topology.logical_processors, 64);
        topology.performance_mask = n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
    }
    return topology;
}

#endif

// ---------------------------------------------------------------------------------------------
// Launching

std::vector<std::string> BuildArguments(const fs::path& game, const LaunchOptions& options) {
    std::vector<std::string> args;
    if (options.fullscreen) {
        args.emplace_back("-f");
    }
    if (!options.user.empty()) {
        args.emplace_back("-u");
        args.push_back(options.user);
    }
    for (const std::string& extra : options.extra_args) {
        if (!extra.empty()) {
            args.push_back(extra);
        }
    }
    if (!game.empty()) {
        args.emplace_back("-g");
        args.push_back(PathToUtf8(game));
    }
    return args;
}

std::string QuoteWindowsArgument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(argument);
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

#ifdef _WIN32

namespace {
std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string LastErrorText() {
    const DWORD code = GetLastError();
    LPSTR message = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    std::string text = message ? message : "error " + std::to_string(code);
    if (message) {
        LocalFree(message);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}
} // namespace

LaunchResult LaunchEmulator(const fs::path& emulator, const fs::path& game,
                            const LaunchOptions& options) {
    LaunchResult result;
    std::string command = QuoteWindowsArgument(PathToUtf8(emulator));
    for (const std::string& arg : BuildArguments(game, options)) {
        command += ' ';
        command += QuoteWindowsArgument(arg);
    }
    std::wstring command_w = Widen(command);
    const std::wstring working_dir = emulator.parent_path().wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (options.high_priority) {
        flags |= HIGH_PRIORITY_CLASS;
    }
    if (!CreateProcessW(emulator.wstring().c_str(), command_w.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, working_dir.c_str(), &startup, &process)) {
        result.error = "Could not start Eglantine: " + LastErrorText();
        return result;
    }

    if (options.performance_cores_only) {
        const CpuTopology topology = DetectCpuTopology();
        if (topology.hybrid && topology.performance_mask != 0) {
            SetProcessAffinityMask(process.hProcess,
                                   static_cast<DWORD_PTR>(topology.performance_mask));
        }
    }
    if (options.disable_power_throttling) {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0; // Control it, and turn it off.
        SetProcessInformation(process.hProcess, ProcessPowerThrottling, &state, sizeof(state));
    }

    ResumeThread(process.hThread);
    result.ok = true;
    result.pid = static_cast<std::int64_t>(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

bool WaitForExit(std::int64_t pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    return true;
}

#else

LaunchResult LaunchEmulator(const fs::path& emulator, const fs::path& game,
                            const LaunchOptions& options) {
    LaunchResult result;
    std::vector<std::string> args = BuildArguments(game, options);
    args.insert(args.begin(), PathToUtf8(emulator));
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int error = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error != 0) {
        result.error = std::string("Could not start Eglantine: ") + std::strerror(error);
        return result;
    }
#ifdef __linux__
    if (options.performance_cores_only) {
        const CpuTopology topology = DetectCpuTopology();
        if (topology.hybrid) {
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int cpu = 0; cpu < 64; ++cpu) {
                if ((topology.performance_mask >> cpu) & 1) {
                    CPU_SET(cpu, &set);
                }
            }
            sched_setaffinity(pid, sizeof(set), &set);
        }
    }
#endif
    result.ok = true;
    result.pid = pid;
    return result;
}

bool WaitForExit(std::int64_t pid) {
    int status = 0;
    return waitpid(static_cast<pid_t>(pid), &status, 0) == static_cast<pid_t>(pid);
}

#endif

} // namespace Hub
