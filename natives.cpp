#include <iostream>
#include "runtime_interface.h"
#include "natives.h"
Value native_print(const std::vector<Value>& args,const FNctx& ctx){
    Value printable=args[0];
    std::string printstring;
    switch (printable.type){
        case ValueType::String: {
            printstring=std::get<std::string>(printable.value);
            break;
        }
        case ValueType::Int: {
            int64_t num=std::get<int64_t>(printable.value);
            printstring=std::to_string(num);
            break;
        }
        case ValueType::UInt: {
            uint64_t num=std::get<uint64_t>(printable.value);
            printstring=std::to_string(num);
            break;
        }
        case ValueType::Float: {
            double num=std::get<double>(printable.value);
            printstring=std::to_string(num);
            break;
        }
        case ValueType::Null: {
            printstring="null";
            break;
        }
        case ValueType::Bool: {
            bool value=std::get<bool>(printable.value);
            printstring=value?"true":"false";
            break;
        }
        default: {
            printstring="unknown";
        }
    }
    std::cout << printstring << std::endl;
    return Value{ValueType::Null,std::monostate{}};
}
void initNatives(){
    registerNative("print",{ValueType::Any},ValueType::Null,*native_print);
}