#include "xau/mapped_file.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace xau {
namespace {

[[noreturn]] void fail(const std::filesystem::path& p, const char* what) {
    throw std::runtime_error("MappedFile: " + std::string(what) + ": " + p.string());
}

}  // namespace

#ifdef _WIN32

MappedFile::MappedFile(const std::filesystem::path& path) {
    HANDLE fh = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (fh == INVALID_HANDLE_VALUE) fail(path, "cannot open");
    file_ = fh;

    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(fh, &sz)) { close_(); fail(path, "cannot stat"); }
    size_ = static_cast<std::size_t>(sz.QuadPart);
    opened_ = true;

    if (size_ == 0) return;   // empty file: opened, nothing to map

    HANDLE mh = ::CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mh == nullptr) { close_(); fail(path, "cannot create mapping"); }
    mapping_ = mh;

    void* view = ::MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) { close_(); fail(path, "cannot map view"); }
    data_ = static_cast<const std::byte*>(view);
}

void MappedFile::close_() noexcept {
    if (data_)    { ::UnmapViewOfFile(data_); data_ = nullptr; }
    if (mapping_) { ::CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (file_)    { ::CloseHandle(static_cast<HANDLE>(file_));    file_ = nullptr; }
    size_ = 0;
    opened_ = false;
}

void MappedFile::advise_sequential() const noexcept {
    // FILE_FLAG_SEQUENTIAL_SCAN at open time already tells the cache manager
    // what it needs; PrefetchVirtualMemory would only duplicate that.
}

#else   // POSIX

MappedFile::MappedFile(const std::filesystem::path& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) fail(path, "cannot open");

    struct stat st {};
    if (::fstat(fd_, &st) != 0) { close_(); fail(path, "cannot stat"); }
    size_ = static_cast<std::size_t>(st.st_size);
    opened_ = true;

    if (size_ == 0) return;

    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) { close_(); fail(path, "cannot mmap"); }
    data_ = static_cast<const std::byte*>(p);
}

void MappedFile::close_() noexcept {
    if (data_) { ::munmap(const_cast<std::byte*>(data_), size_); data_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    size_ = 0;
    opened_ = false;
}

void MappedFile::advise_sequential() const noexcept {
    if (data_ && size_) {
        ::madvise(const_cast<std::byte*>(data_), size_, MADV_SEQUENTIAL);
    }
}

#endif

MappedFile::~MappedFile() { close_(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      opened_(std::exchange(other.opened_, false))
#ifdef _WIN32
    , file_(std::exchange(other.file_, nullptr)),
      mapping_(std::exchange(other.mapping_, nullptr))
#else
    , fd_(std::exchange(other.fd_, -1))
#endif
{}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close_();
        data_   = std::exchange(other.data_, nullptr);
        size_   = std::exchange(other.size_, 0);
        opened_ = std::exchange(other.opened_, false);
#ifdef _WIN32
        file_    = std::exchange(other.file_, nullptr);
        mapping_ = std::exchange(other.mapping_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

}  // namespace xau
