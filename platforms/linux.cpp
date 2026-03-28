#include "../crossplatform_utils.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <memory>
#include <vector>
namespace Crossplatform {
    MappedFile::~MappedFile() {
        if (data) ::munmap(data, size);
        if (fd >= 0) close(fd);
    }
    std::span<uint8_t> MappedFile::span(){
    	return {data,size};
    }
    MappedFile::MappedFile(MappedFile&& other) noexcept : data(other.data), size(other.size), fd(other.fd) {
	    other.data = nullptr;
	    other.size = 0;
	    other.fd = -1;
	}
	MappedFile::MappedFile() : data(nullptr), size(0), fd(-1) {}
	MappedFile mmap(const std::string& filename) {
	    MappedFile file;

	    file.fd = open(filename.c_str(), O_RDONLY);
	    if (file.fd < 0){
	    	throw std::runtime_error("Failed to open file");
	    }

	    off_t sz = lseek(file.fd, 0, SEEK_END);
	    if (sz == -1){
	    	throw std::runtime_error("Failed to get file size");
	    }
	    file.size = static_cast<size_t>(sz);
	    lseek(file.fd, 0, SEEK_SET);

	    file.data = static_cast<uint8_t*>(::mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, file.fd, 0));
	    if (file.data == MAP_FAILED){
	    	throw std::runtime_error("Failed to mmap file");
	    } 

	    return file;
	}
	fs::path get_selfpath(){
		return fs::read_symlink("/proc/self/exe");
	}
}