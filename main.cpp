#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include "filereader.h"
#include <array>
#include <vector>
#include <string>
#include "crossplatform_utils.h"
#include <iostream>
#include <fstream>
#include "propellant.h"
#include <format>
#include <filesystem>
//Make PPTR header available
//some trash for Windows :(
#if defined(_MSC_VER)
    #pragma section(".sea", read)
    #define SECTION_SEA __declspec(allocate(".sea")) __declspec(selectany)
    #define SECTION_USED
#elif defined(__GNUC__) || defined(__clang__)
    #define SECTION_SEA __attribute__((section(".sea")))
    #define SECTION_USED __attribute__((used))
#else
    #error "Unsupported compiler for custom section"
#endif
SECTION_SEA alignas(8) volatile uint64_t PPTR SECTION_USED=0x7a7a7a7a7a7a7a7a;
SECTION_SEA alignas(8) volatile uint64_t FLAGS SECTION_USED=0xACACACACACACACAC;
uint64_t orig_PPTR=0x7a7a7a7a7a7a7a7a;
enum Flags : uint64_t {
    FLAG_USERMODE=1<<0
};
[[noreturn]] void PRETTY_ERROR(int code, std::string error){
    fprintf(stderr, "%s\n", error.c_str());
    std::exit(code);
}
std::array<uint8_t, 8> split(uint64_t num)
{
    std::array<uint8_t, 8> bytes;

    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<uint8_t>((num >> (i * 8)) & 0xFF);

    return bytes;
}
std::optional<std::string> get_env(const std::string& name) {
    if (const char* val = std::getenv(name.c_str()))
        return std::string(val);
    return std::nullopt;
}
bool set_pptr(FileReader& file, uint64_t value){
    auto splitPPTR=split(PPTR);
    auto splitTarget=split(value);
    file.seek(0);
    size_t count=0;
    while (!file.eof()){//will read 1 byte past EOF as a 0, thats fine as PPTR doesnt have any 0's in it
        uint8_t next=file.next();
        if (next==splitPPTR[count]){
            if (++count==8){
                file.seek(file.pos()-8);
                count=0;
                while (count!=8){
                    file.set(splitTarget[count++]);
                }
                return true;
            }
        } else {
            count=0;
        }
    }
    return false;
}
bool set_flags(FileReader& file, uint64_t value){
    auto splitFLAGS=split(FLAGS);
    auto splitTarget=split(value);
    file.seek(0);
    size_t count=0;
    while (!file.eof()){//will read 1 byte past EOF as a 0, thats fine as PPTR doesnt have any 0's in it
        uint8_t next=file.next();
        if (next==splitFLAGS[count]){
            if (++count==8){
                file.seek(file.pos()-8);
                count=0;
                while (count!=8){
                    file.set(splitTarget[count++]);
                }
                return true;
            }
        } else {
            count=0;
        }
    }
    return false;
}
struct PatchOpts {
    std::string propellant_filepath;
    uint64_t flags;
};
void patch_self(PatchOpts& opts){
    std::ifstream prop;
    std::ifstream self;
    std::ofstream output;
    fs::path selfpath=Crossplatform::get_selfpath();
    fs::path prop_path=opts.propellant_filepath;
    prop.open(prop_path.string(),std::ios::in | std::ios::binary);
    self.open(selfpath.string(),std::ios::in  | std::ios::binary);
    fs::path outputPath=prop_path.parent_path() / prop_path.stem();
    output.open(outputPath,std::ios::out  | std::ios::binary);
    output << self.rdbuf();//copy thruster to output
    std::streampos pos=output.tellp();
    output << prop.rdbuf();
    std::streampos posEnd=output.tellp();
    output.flush();
    output.close();
    std::filesystem::permissions(outputPath,std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add);
    self.close();
    prop.close();
    FileReader outRW;
    outRW.open(outputPath.string().c_str());
    uint64_t offset = static_cast<uint64_t>(pos);
    set_pptr(outRW,offset);
    set_flags(outRW,opts.flags);
    outRW.close();
    std::cout << "patched, output is" << outputPath.string() << std::endl;
}
bool hasFlag(uint64_t flag){
    return (FLAGS & flag) != 0;
}
template<typename T>
int indexOfVector(const std::span<T>& span, const T& value){
    for (size_t i=0;i < span.size();i++){
        if (span[i]==value) return i;
    }
    return -1;
}
struct ExecutionError {
    uint64_t func_id;
    std::string message;
};
void executePropellantWrapper(PropellantLoadData& data, std::span<std::string> args, bool useDebugSyms){
    try {
        executePropellant(data,args,useDebugSyms);
    } catch (const ExecutionError& e){
        if (e.func_id==0xffffffffffffffff){
            PRETTY_ERROR(1,std::format("Error during execution of entry function: {}",e.message));
        } else {
            PRETTY_ERROR(1,std::format("Error during execution of function {}: {}",e.func_id,e.message));
        }
    } catch (std::runtime_error& e){
        PRETTY_ERROR(1,e.what());
    }
}
//main
int main(int argc,char** argv){
    std::vector<std::string> args_raw(argv, argv+ argc);
    std::span<std::string> args(args_raw);
    bool print_sea_data=get_env("THRUSTER_SEADATA").has_value();
    bool debug_mode=get_env("THRUSTER_DEBUGSYM").has_value();
    if (print_sea_data){
        std::cerr << std::format("SEA data:\n\tPPTR: {:#x}, orig_PPTR: {:#x}\n\tFLAGS: {:#x}", PPTR, orig_PPTR, FLAGS) << std::endl;
    }
    if (!hasFlag(Flags::FLAG_USERMODE)){
        int fuelIndex=indexOfVector(args,(std::string)"--fuel");
        if (fuelIndex!=-1){
            PropellantLoadData execData;
            execData.start=0;
            execData.filename=args[fuelIndex+1];
            executePropellantWrapper(execData, args.subspan(fuelIndex+2), debug_mode);
            return 0;
        }
        PatchOpts opts;
        opts.flags |= Flags::FLAG_USERMODE;
        int fillIndex=indexOfVector(args,(std::string)"--fill");
        if (fillIndex!=-1){
            opts.propellant_filepath=args[fillIndex+1];
        }
        int flagsIndex = indexOfVector(args, std::string("--flags"));
        if (flagsIndex != -1 && flagsIndex + 1 < (int)args.size())
        {
            std::string flagsString = args[flagsIndex + 1];
            uint64_t parsedFlags = 0;
            if (flagsString.size() >= 2 && flagsString[0] == '0')
            {
                if (flagsString[1] == 'x'){
                    parsedFlags = std::stoull(flagsString.substr(2), nullptr, 16);
                } else if (flagsString[1] == 'b'){
                    parsedFlags = std::stoull(flagsString.substr(2), nullptr, 2);
                } else {
                    PRETTY_ERROR(1, "Invalid flags string");
                }
            } else {
                parsedFlags = std::stoull(flagsString, nullptr, 10);
            }
            opts.flags = parsedFlags;
        }
        patch_self(opts);

    }
    if (PPTR!=orig_PPTR){
        PropellantLoadData execData;
        execData.start=PPTR;
        execData.filename=Crossplatform::get_selfpath();
        executePropellantWrapper(execData, args.subspan(1), debug_mode);
        return 0;
    }
	return 1;
}