#include <cstdint>
#include <filesystem>
#include <span>
struct PropellantLoadData {
    std::filesystem::path filename;
    size_t start;
};
void executePropellant(PropellantLoadData& data, std::span<std::string>& args, bool useDebugSyms);