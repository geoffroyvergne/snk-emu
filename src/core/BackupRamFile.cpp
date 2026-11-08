#include "core/BackupRamFile.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace snk::backup_ram {

namespace fs = std::filesystem;

LoadResult load(const fs::path& path, std::span<uint8_t> ram) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return LoadResult::Missing;
    }
    if (fs::file_size(path, ec) != ram.size() || ec) {
        return LoadResult::WrongSize;
    }
    std::vector<uint8_t> file(ram.size());
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()))) {
        return LoadResult::WrongSize;
    }
    for (size_t i = 0; i < ram.size(); ++i) {
        ram[i] = file[i ^ 1u];  // little-endian words -> 68000 order
    }
    return LoadResult::Loaded;
}

std::optional<fs::file_time_type> modificationTime(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return time;
}

void save(const fs::path& path, std::span<const uint8_t> ram) {
    std::vector<uint8_t> file(ram.size());
    for (size_t i = 0; i < ram.size(); ++i) {
        file[i] = ram[i ^ 1u];  // 68000 order -> little-endian words
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()))) {
            throw std::runtime_error(temp.string() + ": write failed");
        }
    }
    fs::rename(temp, path, ec);  // atomic replace: a crash never leaves a half-written save
    if (ec) {
        const std::string reason = ec.message();
        fs::remove(temp, ec);
        throw std::runtime_error(path.string() + ": " + reason);
    }
}

}  // namespace snk::backup_ram
