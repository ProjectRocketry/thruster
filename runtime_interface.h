#include "../libpropfile/prop_hc_interface.h"
//runtime structs
struct ExecutionError {
    ExecutionError(uint64_t fid, std::string msg) : func_id(fid), message(msg) {}
    uint64_t func_id;
    std::string message;
};
struct FrozenFunction {
	LinkedPropFunction function;
	std::unordered_map<uint64_t,size_t> labelOffsets;
};
template<typename T>
struct BoundedVector {
    std::vector<T> data;
    size_t max_size;
    uint64_t id;

    void push_back(const T& value) {
        if (data.size() >= max_size) {
            throw ExecutionError(id,"Stack overflow");
        }
        data.push_back(value);
    }

    T pop_back() {
        if (data.empty()) {
            throw ExecutionError(id, "Stack underflow");
        }
        T v = std::move(data.back());
        data.pop_back();
        return v;
    }

    T& back() { return data.back(); }
    size_t size() const { return data.size(); }
    bool empty() { return data.empty(); }
    std::vector<T>::iterator begin(){ return data.begin(); }
    std::vector<T>::iterator end(){ return data.end(); }
    T& at(size_t size){ return data.at(size); }
};
using Stack=BoundedVector<Value>;
struct FNctx {
    FrozenFunction& func;
    Stack& stack;
    std::vector<Value>& args;
    std::map<uint64_t,LinkedPropFunction>& functions;
    std::map<uint64_t,std::string>& stringTable;
    size_t& pc;
    std::map<uint64_t,Value> vars;
    uint64_t id;
};
using NativeSig=Value(*)(const std::vector<Value>&,const FNctx&);
struct NativeEntry {
    NativeSig fn;
    std::vector<ValueType> argTypes;
    ValueType retType;
};
using Registrar=void(*)(std::string name, std::vector<ValueType> argTypes, ValueType retType, NativeSig fn);
void registerNative(std::string name, std::vector<ValueType> argTypes, ValueType retType, NativeSig fn);