#include <filesystem>
#include <cstdint>
#include <string>
#include <span>
namespace fs=std::filesystem;
namespace Crossplatform {
	struct MappedFile {
	    uint8_t* data;
	    size_t size;
	    int fd;
	    ~MappedFile();
	    MappedFile();
	    MappedFile(const MappedFile&) = delete;
	    MappedFile& operator=(const MappedFile&) = delete;
	    MappedFile(MappedFile&& other) noexcept;
	    MappedFile& operator=(MappedFile&& other) = delete;
	    std::span<uint8_t> span();
	};
	fs::path get_selfpath();
	MappedFile mmap(const std::string& filename);
}