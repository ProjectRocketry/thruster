#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
/* ============================================================
   Opcode table (single source of truth)
   ============================================================ */

struct OpcodeEntry {
    uint8_t opcode;
    const char* primary;                  // canonical mnemonic
    std::vector<const char*> aliases;     // alternative spellings
};

/* 
   NOTE:
   - `primary` is what the decompiler prints
   - `aliases` are accepted by the compiler
   - `primary` is NOT duplicated in aliases
*/
static const std::vector<OpcodeEntry> OPCODES = {
    {0x10, "loadint",    {}},
    {0x11, "loadstring", {}},//no impl yet
    {0x12, "loadfloat",  {}},
    {0x13, "loadvar",    {}},
    {0x14, "loadarg",    {}},
    {0x15, "loadbool",   {}},

    {0x20, "savevar",    {}},
    {0x21, "clone",      {}},
    {0x22, "pop",        {}},
    {0x23, "incr",       {"++"}},
    {0x24, "decr",       {"--"}},

    {0x30, "branchlabel", {}},
    {0x31, "jump",        {}},
    {0x32, "jumpiftrue",  {}},
    {0x33, "jumpiffalse", {}},
    {0x34, "call",        {}},
    {0x35, "callnative",  {}},
    {0x36, "callextern",  {}},//no impl yet
    {0x37, "return",      {}},
    {0x38, "exit",        {}},

    {0x40, "add",  {"+"}},
    {0x41, "sub",  {"-"}},
    {0x42, "mult", {"*"}},
    {0x43, "div",  {"/"}},
    {0x44, "mod",  {"%"}},

    {0x60, "eq", {"=="}},
    {0x61, "ne", {"!="}},
    {0x62, "lt", {"<"}},
    {0x63, "le", {"<="}},
    {0x64, "gt", {">"}},
    {0x65, "ge", {">="}},

    {0x70, "not", {"!"}},
    {0x71, "and", {"&&"}},
    {0x72, "or",  {"||"}},
};
static std::unordered_map<std::string, uint8_t> TYPE_CODES = {
    {"void",0x00},
    {"int",0x01},
    {"float",0x02},
    {"bool",0x03},
    {"string",0x04},
    {"int[]",0x11},
    {"float[]",0x12},
    {"bool[]",0x13},
    {"string[]",0x14},
    {"entry",0xFF}
};
/* ============================================================
   Compiler side: mnemonic -> opcode
   ============================================================ */
inline uint8_t opcodeFor(const std::string& mnemonic) {
    for (const auto& op : OPCODES) {
        if (mnemonic == op.primary)
            return op.opcode;

        for (const char* alias : op.aliases) {
            if (mnemonic == alias)
                return op.opcode;
        }
    }

    std::fprintf(stderr, "Unknown mnemonic: %s\n", mnemonic.c_str());
    std::exit(1);
}
inline uint8_t typecodeFor(const std::string& type){
    if (TYPE_CODES.contains(type)){
        uint8_t retval=TYPE_CODES.find(type)->second;
        return retval;
    }
    std::fprintf(stderr, "Unknown typecode: %s\n", type.c_str());
    std::exit(1);
}
/* ============================================================
   Decompiler side: opcode -> canonical mnemonic
   ============================================================ */

inline const char* mnemonic(uint8_t opcode) {
    for (const auto& op : OPCODES) {
        if (op.opcode == opcode)
            return op.primary;
    }
    return "UNKNOWN";
}
inline const std::string getTypename(uint8_t typecode){
    for (auto& [name,code] : TYPE_CODES){
        if (typecode==code){
            return name;
        }
    }
    return "UNKNOWN";
}