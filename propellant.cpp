#include "propellant.h"
#include "crossplatform_utils.h"
#include <unordered_map>
#include <optional>
#include "../libpropfile/prop_hc_interface.h"
#include "../libpropfile/propfile.h"
#include "../libpropfile/opcodes.h"
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <format>
#include <concepts>
#include "runtime_interface.h"
#include <iostream>
#include "natives.h"
size_t MAX_RECURSION=0xFFFF;
size_t MAX_STACK_SIZE=0xFFFF;
size_t depth=0;
using Null=std::monostate;
Value executeFunction(uint64_t id, std::vector<Value> args, std::map<uint64_t,LinkedPropFunction>& functions, std::map<uint64_t,std::string>& stringTable);
Value pop(Stack& v) {
    if (v.empty()) {
        throw std::runtime_error("pop from empty vector");
    }
    Value val = std::move(v.back()); // move if possible
    v.pop_back();                 // remove last element
    return val;
}
void move_to_back(Stack& v, size_t index) {
    if (index >= v.size()) {
        throw std::out_of_range("Index out of range");
    }
    if (index == v.size() - 1) return; // already at the back

    // Rotate the range [index, back] so that the element at index goes to the back
    std::rotate(v.begin() + index, v.begin() + index + 1, v.end());
}
template<typename T>
concept Numeric = std::same_as<T, int64_t> || 
                  std::same_as<T, uint64_t> || 
                  std::same_as<T, double>;
template <typename T>
T getVariantOrThrow(const Value& arg, uint64_t id){
    try {
        return std::get<T>(arg.value);
    } catch (std::bad_variant_access& e){
        throw ExecutionError(id, "Stack top has type "+ValueTypeAsString.at(arg.type)+", expected type "+ValueTypeAsString.at(static_cast<ValueType>(arg.value.index()))+".");
    }
}
template <Numeric T, Numeric N>
T castIfInLimit(N number, uint64_t id){
    using TL = std::numeric_limits<T>;
    bool isSafe;
    if constexpr (std::is_floating_point_v<T> || std::is_floating_point_v<N>) {
        double val = static_cast<double>(number);
        if (!std::isfinite(val)) {
            isSafe = false;
        } else {
            isSafe = val >= static_cast<double>(TL::lowest()) &&
                     val <= static_cast<double>(TL::max());
        }
    } else if constexpr (std::is_signed_v<N> == std::is_signed_v<T>) {
        isSafe = number >= TL::min() && number <= TL::max();
    } else if constexpr (std::is_signed_v<N>) {
        if (number < 0) {
            isSafe = false;
        } else {
            using U = std::make_unsigned_t<N>;
            isSafe = static_cast<U>(number) <= TL::max();
        }
    } else {
        using U = std::make_unsigned_t<T>;
        isSafe = number <= static_cast<U>(TL::max());
    }
    if (!isSafe)
        throw ExecutionError(id, "Number went out of bounds.");
    return static_cast<T>(number);
}
template <Numeric T>
T parseString(const std::string& str, uint64_t id){
    try {
        if constexpr (std::same_as<T, int64_t>) {
            size_t pos;
            int64_t val = std::stoll(str, &pos);
            if (pos != str.size())
                throw std::invalid_argument("Trailing characters");
            return val;

        } else if constexpr (std::same_as<T, uint64_t>) {
            size_t pos;
            uint64_t val = std::stoull(str, &pos);
            if (pos != str.size())
                throw std::invalid_argument("Trailing characters");
            return val;

        } else if constexpr (std::same_as<T, double>) {
            size_t pos;
            double val = std::stod(str, &pos);
            if (pos != str.size())
                throw std::invalid_argument("Trailing characters");

            // Reject NaN explicitly
            if (std::isnan(val))
                throw std::invalid_argument("NaN not allowed");

            return val;
        }
    } catch (...) {
        throw ExecutionError(id, std::format("Could not convert string {} to number", str));
    }
}
std::vector<Value> resolveArgsForCall(std::vector<ValueType>& argTypes, FNctx& ctx){
    std::vector<Value> ret;
    std::vector<Value> temp;
    size_t i=0;
    for (ValueType& argtype : argTypes){
        Value top=pop(ctx.stack);
        if (top.type==ValueType::Null){
            temp.push_back(top);
            i++;
        } else if (top.type!=argtype && argtype!=ValueType::Any) {
            throw ExecutionError(ctx.id,"Stack top has type "+ValueTypeAsString.at(top.type)+", expected type "+ValueTypeAsString.at(argtype)+".");
        } else {
            ret.push_back(top);
        }
    }
    for (;i!=0;i--){
        Value top=temp.back();
        temp.pop_back();
        ctx.stack.push_back(top);
    }
    return ret;
}
template<typename T>
Value mkValue(T t){
    Value v;
    v.value=t;
    v.type=static_cast<ValueType>(v.value.index());
    return v;
}
bool isNumberValueType(ValueType v){
    return (v == ValueType::Int || v == ValueType::UInt || v == ValueType::Float);
}
Value coerce(const Value& arg, ValueType target, uint64_t ctx);
std::pair<Value,Value> promote(Value& a1,Value& a2, uint64_t id){
    ValueType coerceTarget;
    if (a1.type==ValueType::Float||a2.type==ValueType::Float){
        coerceTarget=ValueType::Float;
    } else if (a1.type==ValueType::UInt||a2.type==ValueType::UInt){
        coerceTarget=ValueType::UInt;
    } else {
        coerceTarget=ValueType::Int;
    }
    Value b1=coerce(a1,coerceTarget,id);
    Value b2=coerce(a2,coerceTarget,id);
    return {b1,b2};
}
bool valuesEqual(Value& a1, Value& a2, uint64_t id){
    std::pair<Value,Value> normalized;
    if (a1.type==ValueType::Null || a2.type == ValueType::Null){
            return false;
        }
    if (a1.type!=a2.type){
        if (isNumberValueType(a1.type) && isNumberValueType(a2.type)){//isNumberValueType is true if Int,UInt, or Float
            normalized=promote(a1,a2,id);
        } else if (a1.type == ValueType::String || a2.type == ValueType::String){
            normalized={coerce(a1,ValueType::String,id),coerce(a2,ValueType::String,id)};
        } else if (a1.type==ValueType::Bool && isNumberValueType(a2.type)){
            Value num=coerce(a1,ValueType::UInt,id);
            normalized=promote(num,a2,id);
        } else if (isNumberValueType(a1.type) && a2.type==ValueType::Bool){
            Value num=coerce(a2,ValueType::UInt,id);
            normalized=promote(a1,num,id);
        }
    } else {
        normalized={a1,a2};
    }
    bool retval;
    switch (normalized.first.type){
        case ValueType::Float:
            retval=getVariantOrThrow<double>(normalized.first,id)==getVariantOrThrow<double>(normalized.second,id);
            break;
        case ValueType::Int:
            retval=getVariantOrThrow<int64_t>(normalized.first,id)==getVariantOrThrow<int64_t>(normalized.second,id);
            break;
        case ValueType::UInt:
            retval=getVariantOrThrow<uint64_t>(normalized.first,id)==getVariantOrThrow<uint64_t>(normalized.second,id);
            break;
        case ValueType::String:
            retval=getVariantOrThrow<std::string>(normalized.first,id)==getVariantOrThrow<std::string>(normalized.second,id);
            break;
        case ValueType::Bool:
            retval=getVariantOrThrow<bool>(normalized.first,id)==getVariantOrThrow<bool>(normalized.second,id);
            break;
        default:
            retval=false;
    }
    return retval;
}
/* VM helpers */
void jumpTo(uint64_t label, FNctx& ctx){
    try {
        ctx.pc=ctx.func.labelOffsets.at(label);
    } catch (std::out_of_range& e){
        throw ExecutionError(ctx.id,"Label not found");
    }
}
Value coerce(const Value& arg, ValueType target, uint64_t id){
    if (arg.type==target) return arg;
    switch (target){
        case ValueType::Null:
            return mkValue(std::monostate{});
        case ValueType::Bool: {
            switch (arg.type){
                case ValueType::Int: {
                    return mkValue(getVariantOrThrow<int64_t>(arg,id)!=0);
                }
                case ValueType::UInt: {
                    return mkValue(getVariantOrThrow<uint64_t>(arg,id)!=0);
                }
                case ValueType::Float: {
                    return mkValue(getVariantOrThrow<double>(arg,id)!=0);
                }
                case ValueType::String: {
                    std::string item=getVariantOrThrow<std::string>(arg,id);
                    bool retval;
                    if (item=="true"){
                        retval=true;
                    } else if (item=="false"){
                        retval=false;
                    } else if (!item.empty()){
                        retval=true;
                    } else {
                        retval=false;
                    }
                    return mkValue(retval);
                }
                case ValueType::Null: {
                    return mkValue(false);
                } 
                default:
                    throw ExecutionError(id, "Invalid coercion");
            }
            break;
        }
        case ValueType::Int: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg,id)?1ll:0ll);
                    break;
                }
                case ValueType::UInt: {
                    return mkValue(castIfInLimit<int64_t>(getVariantOrThrow<uint64_t>(arg,id),id));
                }
                case ValueType::Float: {
                    return mkValue(castIfInLimit<int64_t>(getVariantOrThrow<double>(arg,id),id));
                }
                case ValueType::String: {
                    return mkValue(parseString<int64_t>(getVariantOrThrow<std::string>(arg,id),id));
                }
                case ValueType::Null: {
                    return mkValue(0ll);
                } 
                default:
                    throw ExecutionError(id, "Invalid coercion");
            }
            break;
        }
        case ValueType::UInt: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg,id)?1ull:0ull);
                }
                case ValueType::Int: {
                    return mkValue(castIfInLimit<uint64_t>(getVariantOrThrow<int64_t>(arg,id),id));
                }
                case ValueType::Float: {
                    return mkValue(castIfInLimit<uint64_t>(getVariantOrThrow<double>(arg,id),id));
                }
                case ValueType::String: {
                    return mkValue(parseString<uint64_t>(getVariantOrThrow<std::string>(arg,id),id));
                }
                case ValueType::Null: {
                    return mkValue(0ull);
                } 
                default:
                    throw ExecutionError(id, "Invalid coercion");
            }
            break;
        }
        case ValueType::Float: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg,id)?1.0:0.0);
                }
                case ValueType::Int: {
                    return mkValue(castIfInLimit<double>(getVariantOrThrow<int64_t>(arg,id),id));
                }
                case ValueType::UInt: {
                    return mkValue(castIfInLimit<double>(getVariantOrThrow<uint64_t>(arg,id),id));
                }
                case ValueType::String: {
                    return mkValue(parseString<double>(getVariantOrThrow<std::string>(arg,id),id));
                }
                case ValueType::Null: {
                    return mkValue(0.0);
                }
                default:
                    throw ExecutionError(id, "Invalid coercion");
            }
            break;
        }
        case ValueType::String: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg,id)?"true":"false");
                }
                case ValueType::Int: {
                    return mkValue(std::to_string(getVariantOrThrow<int64_t>(arg,id)));
                }
                case ValueType::UInt: {
                    return mkValue(std::to_string(getVariantOrThrow<uint64_t>(arg,id)));
                }
                case ValueType::Float: {
                    return mkValue(std::to_string(getVariantOrThrow<double>(arg,id)));
                }
                case ValueType::Null: {
                    return mkValue("null");
                } 
                default:
                    throw ExecutionError(id, "Invalid coercion");
            }
            break;
        }
        default:
            throw ExecutionError(id, "Invalid coercion");
    }
    return arg;
}
std::unordered_map<uint64_t,NativeEntry> callnatives={};
uint64_t hash64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
void registerNative(std::string name, std::vector<ValueType> argTypes, ValueType retType, NativeSig fn){
    NativeEntry entry;
    entry.argTypes=argTypes;
    entry.retType=retType;
    entry.fn=fn;
    callnatives[hash64(name)]=entry;
}
/* Handlers */
void LoadInt(const LinkedInstruction& instr, FNctx& ctx){
    int64_t i=std::bit_cast<int64_t>(instr.operand);
    ctx.stack.push_back(mkValue(i));
    return;
}
void LoadString(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(mkValue(ctx.stringTable.at(instr.operand)));
    return;
}
void LoadFloat(const LinkedInstruction& instr, FNctx& ctx){
    double f=std::bit_cast<double>(instr.operand);
    ctx.stack.push_back(mkValue(f));
    return;
}
void LoadVar(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(ctx.vars.at(instr.operand));
    return;
}
void LoadArg(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(ctx.args.at(instr.operand));
    return;
}
void LoadBool(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(mkValue(instr.operand>0));
    return;
}
void LoadUInt(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(mkValue(instr.operand));
    return;
}
void LoadNull(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(mkValue(std::monostate{}));
    return;
}
void SaveVar(const LinkedInstruction& instr, FNctx& ctx){
    ctx.vars[instr.operand]=pop(ctx.stack);
    return;
}
void Clone(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(ctx.stack.back());
    return;
}
void Pop(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.pop_back();
    return;
}
void Incr(const LinkedInstruction& instr, FNctx& ctx){
    Value& var=ctx.vars.at(instr.operand);
    switch (var.type){
        case ValueType::Int: {
            int64_t num=getVariantOrThrow<int64_t>(var,ctx.id);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        case ValueType::UInt: {
            uint64_t num=getVariantOrThrow<uint64_t>(var,ctx.id);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        case ValueType::Float: {
            double num=getVariantOrThrow<double>(var,ctx.id);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        default:
            throw ExecutionError(ctx.id,"Tried to increment a non-numeric variable");
    }
}
void Decr(const LinkedInstruction& instr, FNctx& ctx){
    Value& var=ctx.vars.at(instr.operand);
    switch (var.type){
        case ValueType::Int: {
            int64_t num=getVariantOrThrow<int64_t>(var,ctx.id);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        case ValueType::UInt: {
            uint64_t num=getVariantOrThrow<uint64_t>(var,ctx.id);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        case ValueType::Float: {
            double num=getVariantOrThrow<double>(var,ctx.id);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        default:
            throw ExecutionError(ctx.id,"Tried to decrement a non-numeric variable");
    }
    return;
}
void Top(const LinkedInstruction& instr, FNctx& ctx){
    move_to_back(ctx.stack,static_cast<size_t>(instr.operand));
    return;
}
void CloneToTop(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(ctx.stack.at(instr.operand));
    return;
}
void Jump(const LinkedInstruction& instr, FNctx& ctx){
    jumpTo(instr.operand,ctx);
    return;
}
void JumpIfTrue(const LinkedInstruction& instr, FNctx& ctx){
    Value top=pop(ctx.stack);
    bool topBool=getVariantOrThrow<bool>(top,ctx.id);
    if (topBool){
        jumpTo(instr.operand,ctx);
    }
    return;
}
void JumpIfFalse(const LinkedInstruction& instr, FNctx& ctx){
    Value top=pop(ctx.stack);
    bool topBool=getVariantOrThrow<bool>(top,ctx.id);
    if (!topBool){
        jumpTo(instr.operand,ctx);
    }
    return;
}
void Call(const LinkedInstruction& instr, FNctx& ctx){
    std::vector<ValueType>& argTypes=ctx.functions.at(instr.operand).args;
    std::vector<Value> args=resolveArgsForCall(argTypes,ctx);
    Value retval=executeFunction(instr.operand,args,ctx.functions,ctx.stringTable);
    if (retval.type!=ValueType::Null){
        ctx.stack.push_back(retval);
    }
    return;
}
void CallNative(const LinkedInstruction& instr, FNctx& ctx){
    NativeEntry func=callnatives.at(instr.operand);
    std::vector<Value> args=resolveArgsForCall(func.argTypes,ctx);
    Value retval=func.fn(args,ctx);
    if (retval.type!=ValueType::Null){
        ctx.stack.push_back(retval);
    }
    return;
}
Value Return(const LinkedInstruction& instr, FNctx& ctx){
    if (ctx.func.function.returnType==ValueType::Null){
        if (!ctx.stack.empty()) throw ExecutionError(ctx.id, "Stack overflowed over return");
        return mkValue(std::monostate{});
    }
    Value top=pop(ctx.stack);
    if (!ctx.stack.empty()) throw ExecutionError(ctx.id, "Stack overflowed over return");
    if (top.type!=ctx.func.function.returnType) throw ExecutionError(ctx.id, "In 'return': Stack top has type "+ValueTypeAsString.at(top.type)+", expected type "+ValueTypeAsString.at(ctx.func.function.returnType)+".");
    depth--;
    return top;
}
[[noreturn]] void Exit(const LinkedInstruction& instr, FNctx& ctx){
    Value top=pop(ctx.stack);
    int64_t exit_code=getVariantOrThrow<int64_t>(top,ctx.id);
    std::exit(exit_code);
}
#define NUMERIC_OP(OPERATION) \
if (promoted.first.type==ValueType::Float){\
    double result=getVariantOrThrow<double>(promoted.second,ctx.id) OPERATION getVariantOrThrow<double>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::Int){\
    int64_t result=getVariantOrThrow<int64_t>(promoted.second,ctx.id) OPERATION getVariantOrThrow<int64_t>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::UInt){\
    uint64_t result=getVariantOrThrow<uint64_t>(promoted.second,ctx.id) OPERATION getVariantOrThrow<uint64_t>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
}
#define NUMERIC_CMP(OPERATION) \
if (promoted.first.type==ValueType::Float){\
    bool result=getVariantOrThrow<double>(promoted.second,ctx.id) OPERATION getVariantOrThrow<double>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::Int){\
    bool result=getVariantOrThrow<int64_t>(promoted.second,ctx.id) OPERATION getVariantOrThrow<int64_t>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::UInt){\
    bool result=getVariantOrThrow<uint64_t>(promoted.second,ctx.id) OPERATION getVariantOrThrow<uint64_t>(promoted.first,ctx.id);\
    ctx.stack.push_back(mkValue(result));\
}
void Add(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_OP(+)
    return;
}
void Sub(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_OP(-)
    return;
}
void Mult(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_OP(*)
    return;
}
void Div(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_OP(/)
    return;
}
void Mod(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    if (promoted.first.type==ValueType::Float){
        double result=std::fmod(getVariantOrThrow<double>(promoted.second,ctx.id),getVariantOrThrow<double>(promoted.first,ctx.id));
        ctx.stack.push_back(mkValue(result));
    } else if (promoted.first.type==ValueType::Int){
        int64_t result=getVariantOrThrow<int64_t>(promoted.second,ctx.id)%getVariantOrThrow<int64_t>(promoted.first,ctx.id);
        ctx.stack.push_back(mkValue(result));
    } else if (promoted.first.type==ValueType::UInt){
        uint64_t result=getVariantOrThrow<uint64_t>(promoted.second,ctx.id)%getVariantOrThrow<uint64_t>(promoted.first,ctx.id);
        ctx.stack.push_back(mkValue(result));
    }
    return;
}
void Eq(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool equal=valuesEqual(a1,a2,ctx.id);
    ctx.stack.push_back(mkValue(equal));
    return;
}
void Ne(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool notEqual=!valuesEqual(a1,a2,ctx.id);
    ctx.stack.push_back(mkValue(notEqual));
    return;
}
void Lt(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_CMP(<)
    return;
}
void Le(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_CMP(<=)
    return;
}
void Gt(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_CMP(>)
    return;
}
void Ge(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2,ctx.id);
    NUMERIC_CMP(>=)
    return;
}
void Not(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    switch (a.type){
        case ValueType::String: {
            std::string item=getVariantOrThrow<std::string>(a,ctx.id);
            bool retval;
            if (item=="true"){
                retval=true;
            } else if (item=="false"){
                retval=false;
            } else if (!item.empty()){
                retval=true;
            } else {
                retval=false;
            }
            ctx.stack.push_back(mkValue(retval));
            return;
        }
        default:
            Value b=coerce(a,ValueType::Bool,ctx.id);
            bool item=getVariantOrThrow<bool>(b,ctx.id);
            ctx.stack.push_back(mkValue(!item));
            return;
    }
    return;
}
void And(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool v1=getVariantOrThrow<bool>(a1,ctx.id);
    bool v2=getVariantOrThrow<bool>(a2,ctx.id);
    bool ret=v1&&v2;
    ctx.stack.push_back(mkValue(ret));
    return;
}
void Or(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool v1=getVariantOrThrow<bool>(a1,ctx.id);
    bool v2=getVariantOrThrow<bool>(a2,ctx.id);
    bool ret=v1||v2;
    ctx.stack.push_back(mkValue(ret));
    return;
}
void Coerce(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    Value coerced=coerce(a,static_cast<ValueType>(instr.operand),ctx.id);
    ctx.stack.push_back(coerced);
    return;
}
void Typeof(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    std::string typestring=ValueTypeAsString.at(a.type);
    ctx.stack.push_back(mkValue(typestring));
    return;
}
void IsNull(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    if (a.type==ValueType::Null){
        ctx.stack.push_back(mkValue(true));
    } else {
        ctx.stack.push_back(mkValue(false));
    }
    return;
}
[[maybe_unused]] void JumpLabel(const LinkedInstruction& instr, FNctx& ctx){}//never called
/* Execution */
std::unordered_map<uint64_t,size_t> resolveLabelsInsideFunction(LinkedPropFunction& fn){
    std::unordered_map<uint64_t,size_t> labelOffsets;
    std::vector<LinkedInstruction> newFn;
    for (size_t i = 0; i < fn.code.size(); i++) {
        if (static_cast<Opcode>(fn.code[i].opcode)==Opcode::JumpLabel) {
            if (labelOffsets.contains(fn.code[i].operand)) throw std::runtime_error("Duplicate label");
            labelOffsets.emplace(fn.code[i].operand,newFn.size());
        } else {
            newFn.push_back(fn.code[i]);
        }
    }
    fn.code=std::move(newFn);
    return labelOffsets;
}
std::unordered_map<uint64_t, FrozenFunction> resolved;
FrozenFunction resolveFunction(uint64_t id, std::map<uint64_t,LinkedPropFunction>& functions){
    if (resolved.contains(id)) return resolved.at(id);
    FrozenFunction ret;
    ret.function=functions.at(id);
    // for (const Payload& pld : getPayloadsForFunction(id)){
    //     applyPatchesFromPayload(ret.function,pld);
    // }
    ret.labelOffsets = resolveLabelsInsideFunction(ret.function);
    resolved[id]=ret;
    return ret;
}
Value executeFunction(uint64_t id, std::vector<Value> args, std::map<uint64_t,LinkedPropFunction>& functions, std::map<uint64_t,std::string>& stringTable){
    if (++depth>=MAX_RECURSION){
        throw ExecutionError(id, "Recursion depth exceeded");
    }
    FrozenFunction func=resolveFunction(id,functions);
    size_t i=0;
    Stack stack;
    stack.max_size=MAX_STACK_SIZE;
    stack.id=id;
    FNctx ctx{func,stack,args,functions,stringTable,i,{},id};
    while (ctx.pc<func.function.code.size()){
        LinkedInstruction instr=func.function.code[ctx.pc];
        switch (static_cast<Opcode>(instr.opcode)){
#define GENERATE_CASE(OP, CODE, STR, OPERAND, UNUSED1, UNUSED2, RET) \
            case Opcode::OP: { \
                if constexpr (Opcode::OP==Opcode::Return){ \
                    return Return(instr,ctx); \
                } else { \
                    OP(instr,ctx); \
                    break; \
                } \
            } 
            OPCODE_LIST(GENERATE_CASE)
#undef GENERATE_CASE
            default:
                throw ExecutionError(id,"Unknown opcode 0x"+std::to_string(instr.opcode));
        }
        ctx.pc++;
    }
    throw ExecutionError(id, "Function did not exit or return");
}
std::vector<Value> reformatArgs(std::span<std::string>& args){
    std::vector<Value> v;
    for (auto& arg : args){
        v.push_back(mkValue(arg));
    }
    return v;
}
void executeStripped(StrippedPropellant& prop, std::span<std::string>& args){
    executeFunction(ENTRY_FUNCTION_ID, reformatArgs(args), prop.functions, prop.stringTable);
    return;
}
void debugError(ExecutionError& e,std::map<uint64_t,DebugData> symbols){
    DebugData& data=symbols.at(e.func_id);
    throw std::runtime_error(std::format("Error during execution of function {}: {}",data.functionName,e.message));
}
void executeNormal(LinkedPropellant& prop, std::span<std::string>& args){
    try {
        executeFunction(ENTRY_FUNCTION_ID, reformatArgs(args), prop.functions, prop.stringTable);
        return;
    } catch (ExecutionError& e){
        debugError(e,prop.symbols);
    }
}
void executePropellant(PropellantLoadData& data, std::span<std::string>& args, bool useDebugSyms){
    Crossplatform::MappedFile file=Crossplatform::mmap(data.filename.string());
    std::span<uint8_t> fullFile=file.span();
    std::span<const uint8_t> propAndTrailing=fullFile.subspan(data.start,fullFile.size());
    std::variant<LinkedPropellant, StrippedPropellant> deserialized;
    if (useDebugSyms){
        deserialized=deserialize(propAndTrailing,false);
    } else {
        deserialized=deserialize(propAndTrailing,true);
    }
    initNatives();
    if (std::holds_alternative<StrippedPropellant>(deserialized)){
        StrippedPropellant prop=std::get<StrippedPropellant>(deserialized);
        executeStripped(prop,args);
        return;
    } else {
        LinkedPropellant prop=std::get<LinkedPropellant>(deserialized);
        executeNormal(prop,args);
        return;
    }
}