#include "rom/RomSource.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <miniz.h>

#include "core/RomFile.hpp"

namespace snk::romsrc {

namespace fs = std::filesystem;

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

namespace {

class ZipSource final : public RomSource {
public:
    explicit ZipSource(const fs::path& path) {
        label_ = path.string();
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) {
            throw SourceError(label_ + ": " + ec.message());
        }
        // Open the FILE* ourselves so non-ASCII paths work on Windows too.
#ifdef _WIN32
        file_ = _wfopen(path.c_str(), L"rb");
#else
        file_ = std::fopen(path.c_str(), "rb");
#endif
        if (file_ == nullptr) {
            throw SourceError(label_ + ": cannot open");
        }
        if (!mz_zip_reader_init_cfile(&zip_, file_, size, 0)) {
            const std::string why = mz_zip_get_error_string(mz_zip_get_last_error(&zip_));
            std::fclose(file_);
            file_ = nullptr;
            throw SourceError(label_ + ": not a readable zip archive (" + why + ")");
        }
        open_ = true;

        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip_, i, &st) || st.m_is_directory) {
                continue;
            }
            // Sets are flat, but tolerate a top-level folder inside the zip.
            entries_.push_back({fs::path(st.m_filename).filename().string(), st.m_uncomp_size, st.m_crc32,
                                true, static_cast<uint32_t>(i)});
        }
    }

    ~ZipSource() override {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    ZipSource(const ZipSource&)            = delete;
    ZipSource& operator=(const ZipSource&) = delete;

    std::vector<uint8_t> read(const SourceEntry& entry) override {
        if (entry.size > kMaxFileSize) {
            throw SourceError(label_ + "/" + entry.name + ": file too large");
        }
        std::vector<uint8_t> data(static_cast<size_t>(entry.size));
        // miniz inflates and verifies the CRC-32 stored in the archive.
        if (!mz_zip_reader_extract_to_mem(&zip_, entry.handle, data.data(), data.size(), 0)) {
            throw SourceError(label_ + "/" + entry.name + ": " +
                               mz_zip_get_error_string(mz_zip_get_last_error(&zip_)));
        }
        return data;
    }

private:
    mz_zip_archive zip_{};
    std::FILE*     file_ = nullptr;
    bool           open_ = false;
};

class DirectorySource final : public RomSource {
public:
    explicit DirectorySource(const fs::path& path) {
        label_ = path.string();
        for (const auto& item : fs::directory_iterator(path)) {
            if (!item.is_regular_file()) {
                continue;
            }
            entries_.push_back({item.path().filename().string(), item.file_size(), 0, false,
                                static_cast<uint32_t>(paths_.size())});
            paths_.push_back(item.path());
        }
    }

    std::vector<uint8_t> read(const SourceEntry& entry) override {
        try {
            return readRomFile(paths_[entry.handle], kMaxFileSize);  // message names the file
        } catch (const RomLoadError& e) {
            throw SourceError(e.what());
        }
    }

private:
    std::vector<fs::path> paths_;
};

}  // namespace

std::unique_ptr<RomSource> openSource(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return std::make_unique<DirectorySource>(path);
    }
    if (!fs::exists(path, ec)) {
        throw SourceError(path.string() + ": no such file or directory");
    }
    return std::make_unique<ZipSource>(path);
}

}  // namespace snk::romsrc
