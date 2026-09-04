// Read-only memory-mapped file.
//
// Deliberately dependency-free: libxaucore is what the MT5 bridge DLL links
// against, and every external dependency there is a deployment risk. This is
// ~100 lines of platform code and buys us zero third-party surface.
#pragma once

#include <cstddef>
#include <filesystem>

namespace xau {

class MappedFile {
public:
    MappedFile() noexcept = default;

    // Throws std::runtime_error if the file cannot be opened or mapped.
    // A zero-length file maps successfully to an empty view.
    explicit MappedFile(const std::filesystem::path& path);

    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t      size() const noexcept { return size_; }
    [[nodiscard]] bool             is_open() const noexcept { return opened_; }

    // Hint that access will be front-to-back. Advisory only; never fails.
    void advise_sequential() const noexcept;

private:
    void close_() noexcept;

    const std::byte* data_   = nullptr;
    std::size_t      size_   = 0;
    bool             opened_ = false;
#ifdef _WIN32
    void* file_    = nullptr;   // HANDLE
    void* mapping_ = nullptr;   // HANDLE
#else
    int   fd_      = -1;
#endif
};

}  // namespace xau
