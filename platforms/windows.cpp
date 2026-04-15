#include "../crossplatform_utils.h"
namespace Crossplatform {
    MappedFile::~MappedFile() {
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
	    return file;
	}
	fs::path get_selfpath(){
		return "";
	}
}