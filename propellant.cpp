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
        throw ExecError("pop from empty vector");
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
T getVariantOrThrow(const Value& arg){
    try {
        return std::get<T>(arg.value);
    } catch (std::bad_variant_access& e){
        throw ExecError("Tried to access variant: Stack top has type "+ValueTypeAsString.at(arg.type)+", expected type "+ValueTypeAsString.at(static_cast<ValueType>(arg.value.index()))+".");
    }
}
template <Numeric T, Numeric N>
T castIfInLimit(N number){
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
        throw ExecError("Number went out of bounds.");
    return static_cast<T>(number);
}
template <Numeric T>
T parseString(const std::string& str){
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
        throw ExecError(std::format("Could not convert string {} to number", str));
    }
}
std::vector<Value> resolveArgsForCall(std::vector<ValueType>& argTypes, FNctx& ctx){
    std::vector<Value> ret;
    std::vector<Value> temp;
    size_t i=0;
    for (ValueType& argtype : argTypes){
        Value top=pop(ctx.stack);
        if (argtype==ValueType::Null){
            temp.push_back(top);
            ret.push_back(Value{ValueType::Null,Null{}});//pushes a null to adjust arg indexes
            i++;
        } else if (top.type!=argtype && argtype!=ValueType::Any) {
            throw ExecError("Stack top has type "+ValueTypeAsString.at(top.type)+", expected type "+ValueTypeAsString.at(argtype)+".");
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
Value coerce(const Value& arg, ValueType target);
std::pair<Value,Value> promote(Value& a1, Value& a2){
    ValueType coerceTarget;
    if (a1.type==ValueType::Float||a2.type==ValueType::Float){
        coerceTarget=ValueType::Float;
    } else if (a1.type==ValueType::UInt||a2.type==ValueType::UInt){
        coerceTarget=ValueType::UInt;
    } else {
        coerceTarget=ValueType::Int;
    }
    Value b1=coerce(a1,coerceTarget);
    Value b2=coerce(a2,coerceTarget);
    return {b1,b2};
}
bool valuesEqual(Value& a1, Value& a2, uint64_t id){
    std::pair<Value,Value> normalized;
    if (a1.type==ValueType::Null || a2.type == ValueType::Null){
            return false;
        }
    if (a1.type!=a2.type){
        if (isNumberValueType(a1.type) && isNumberValueType(a2.type)){//isNumberValueType is true if Int,UInt, or Float
            normalized=promote(a1,a2);
        } else if (a1.type == ValueType::String || a2.type == ValueType::String){
            normalized={coerce(a1,ValueType::String),coerce(a2,ValueType::String)};
        } else if (a1.type==ValueType::Bool && isNumberValueType(a2.type)){
            Value num=coerce(a1,ValueType::UInt);
            normalized=promote(num,a2);
        } else if (isNumberValueType(a1.type) && a2.type==ValueType::Bool){
            Value num=coerce(a2,ValueType::UInt);
            normalized=promote(a1,num);
        }
    } else {
        normalized={a1,a2};
    }
    bool retval;
    switch (normalized.first.type){
        case ValueType::Float:
            retval=getVariantOrThrow<double>(normalized.first)==getVariantOrThrow<double>(normalized.second);
            break;
        case ValueType::Int:
            retval=getVariantOrThrow<int64_t>(normalized.first)==getVariantOrThrow<int64_t>(normalized.second);
            break;
        case ValueType::UInt:
            retval=getVariantOrThrow<uint64_t>(normalized.first)==getVariantOrThrow<uint64_t>(normalized.second);
            break;
        case ValueType::String:
            retval=getVariantOrThrow<std::string>(normalized.first)==getVariantOrThrow<std::string>(normalized.second);
            break;
        case ValueType::Bool:
            retval=getVariantOrThrow<bool>(normalized.first)==getVariantOrThrow<bool>(normalized.second);
            break;
        default:
            retval=false;
    }
    return retval;
}
/* VM helpers */
void jumpTo(uint64_t label, FNctx& ctx){
    try {
        ctx.pc=ctx.func.labelOffsets.at(label)-1;//to account for the ctx.pc++ at the end of the loop
    } catch (std::out_of_range& e){
        throw ExecError("Label not found");
    }
}
Value coerce(const Value& arg, ValueType target){
    if (arg.type==target) return arg;
    switch (target){
        case ValueType::Null:
            return mkValue(std::monostate{});
        case ValueType::Bool: {
            switch (arg.type){
                case ValueType::Int: {
                    return mkValue(getVariantOrThrow<int64_t>(arg)!=0);
                }
                case ValueType::UInt: {
                    return mkValue(getVariantOrThrow<uint64_t>(arg)!=0);
                }
                case ValueType::Float: {
                    return mkValue(getVariantOrThrow<double>(arg)!=0);
                }
                case ValueType::String: {
                    std::string item=getVariantOrThrow<std::string>(arg);
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
                    throw ExecError("Invalid coercion");
            }
            break;
        }
        case ValueType::Int: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg)?1ll:0ll);
                    break;
                }
                case ValueType::UInt: {
                    return mkValue(castIfInLimit<int64_t>(getVariantOrThrow<uint64_t>(arg)));
                }
                case ValueType::Float: {
                    return mkValue(castIfInLimit<int64_t>(getVariantOrThrow<double>(arg)));
                }
                case ValueType::String: {
                    return mkValue(parseString<int64_t>(getVariantOrThrow<std::string>(arg)));
                }
                case ValueType::Null: {
                    return mkValue(0ll);
                }
                default:
                    throw ExecError("Invalid coercion");
            }
            break;
        }
        case ValueType::UInt: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg)?1ull:0ull);
                }
                case ValueType::Int: {
                    return mkValue(castIfInLimit<uint64_t>(getVariantOrThrow<int64_t>(arg)));
                }
                case ValueType::Float: {
                    return mkValue(castIfInLimit<uint64_t>(getVariantOrThrow<double>(arg)));
                }
                case ValueType::String: {
                    return mkValue(parseString<uint64_t>(getVariantOrThrow<std::string>(arg)));
                }
                case ValueType::Null: {
                    return mkValue(0ull);
                }
                default:
                    throw ExecError("Invalid coercion");
            }
            break;
        }
        case ValueType::Float: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg)?1.0:0.0);
                }
                case ValueType::Int: {
                    return mkValue(castIfInLimit<double>(getVariantOrThrow<int64_t>(arg)));
                }
                case ValueType::UInt: {
                    return mkValue(castIfInLimit<double>(getVariantOrThrow<uint64_t>(arg)));
                }
                case ValueType::String: {
                    return mkValue(parseString<double>(getVariantOrThrow<std::string>(arg)));
                }
                case ValueType::Null: {
                    return mkValue(0.0);
                }
                default:
                    throw ExecError("Invalid coercion");
            }
            break;
        }
        case ValueType::String: {
            switch (arg.type){
                case ValueType::Bool: {
                    return mkValue(getVariantOrThrow<bool>(arg)?"true":"false");
                }
                case ValueType::Int: {
                    return mkValue(std::to_string(getVariantOrThrow<int64_t>(arg)));
                }
                case ValueType::UInt: {
                    return mkValue(std::to_string(getVariantOrThrow<uint64_t>(arg)));
                }
                case ValueType::Float: {
                    return mkValue(std::to_string(getVariantOrThrow<double>(arg)));
                }
                case ValueType::Null: {
                    return mkValue("null");
                }
                default:
                    throw ExecError("Invalid coercion");
            }
            break;
        }
        default:
            throw ExecError("Cannot convert to type "+std::format("{:#x}",static_cast<int>(target)));
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
    auto i=std::bit_cast<int64_t>(instr.operand);
    ctx.stack.push_back(mkValue(i));
    return;
}
void LoadString(const LinkedInstruction& instr, FNctx& ctx){
    ctx.stack.push_back(mkValue(ctx.stringTable.at(instr.operand)));
    return;
}
void LoadFloat(const LinkedInstruction& instr, FNctx& ctx){
    auto f=std::bit_cast<double>(instr.operand);
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
            auto num=getVariantOrThrow<int64_t>(var);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        case ValueType::UInt: {
            auto num=getVariantOrThrow<uint64_t>(var);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        case ValueType::Float: {
            auto num=getVariantOrThrow<double>(var);
            num++;
            ctx.vars[instr.operand]=mkValue(num);
            return;
        }
        default:
            throw ExecError("Tried to increment a non-numeric variable");
    }
}
void Decr(const LinkedInstruction& instr, FNctx& ctx){
    Value& var=ctx.vars.at(instr.operand);
    switch (var.type){
        case ValueType::Int: {
            auto num=getVariantOrThrow<int64_t>(var);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        case ValueType::UInt: {
            auto num=getVariantOrThrow<uint64_t>(var);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        case ValueType::Float: {
            double num=getVariantOrThrow<double>(var);
            num--;
            ctx.vars[instr.operand]=mkValue(num);
            break;
        }
        default:
            throw ExecError("Tried to decrement a non-numeric variable");
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
    bool topBool=getVariantOrThrow<bool>(top);
    if (topBool){
        jumpTo(instr.operand,ctx);
    }
    return;
}
void JumpIfFalse(const LinkedInstruction& instr, FNctx& ctx){
    Value top=pop(ctx.stack);
    bool topBool=getVariantOrThrow<bool>(top);
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
        if (!ctx.stack.empty()) throw ExecError("Stack overflowed over return");
        return mkValue(std::monostate{});
    }
    Value top=pop(ctx.stack);
    if (!ctx.stack.empty()) throw ExecError("Stack overflowed over return");
    if (top.type!=ctx.func.function.returnType) throw ExecError("In 'return': Stack top has type "+ValueTypeAsString.at(top.type)+", expected type "+ValueTypeAsString.at(ctx.func.function.returnType)+".");
    depth--;
    return top;
}
[[noreturn]] void Exit(const LinkedInstruction& instr, FNctx& ctx){
    Value top=pop(ctx.stack);
    int64_t exit_code=getVariantOrThrow<int64_t>(top);
    std::exit(exit_code);
}
#define NUMERIC_OP(OPERATION) \
if (promoted.first.type==ValueType::Float){\
    double result=getVariantOrThrow<double>(promoted.second) OPERATION getVariantOrThrow<double>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::Int){\
    int64_t result=getVariantOrThrow<int64_t>(promoted.second) OPERATION getVariantOrThrow<int64_t>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::UInt){\
    uint64_t result=getVariantOrThrow<uint64_t>(promoted.second) OPERATION getVariantOrThrow<uint64_t>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
}
#define NUMERIC_CMP(OPERATION) \
if (promoted.first.type==ValueType::Float){\
    bool result=getVariantOrThrow<double>(promoted.second) OPERATION getVariantOrThrow<double>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::Int){\
    bool result=getVariantOrThrow<int64_t>(promoted.second) OPERATION getVariantOrThrow<int64_t>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
} else if (promoted.first.type==ValueType::UInt){\
    bool result=getVariantOrThrow<uint64_t>(promoted.second) OPERATION getVariantOrThrow<uint64_t>(promoted.first);\
    ctx.stack.push_back(mkValue(result));\
}
void Add(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_OP(+)
    return;
}
void Sub(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_OP(-)
    return;
}
void Mult(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_OP(*)
    return;
}
void Div(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_OP(/)
    return;
}
void Mod(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    if (promoted.first.type==ValueType::Float){
        double result=std::fmod(getVariantOrThrow<double>(promoted.second),getVariantOrThrow<double>(promoted.first));
        ctx.stack.push_back(mkValue(result));
    } else if (promoted.first.type==ValueType::Int){
        int64_t result=getVariantOrThrow<int64_t>(promoted.second)%getVariantOrThrow<int64_t>(promoted.first);
        ctx.stack.push_back(mkValue(result));
    } else if (promoted.first.type==ValueType::UInt){
        uint64_t result=getVariantOrThrow<uint64_t>(promoted.second)%getVariantOrThrow<uint64_t>(promoted.first);
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
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_CMP(<)
    return;
}
void Le(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_CMP(<=)
    return;
}
void Gt(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_CMP(>)
    return;
}
void Ge(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    std::pair<Value,Value> promoted=promote(a1,a2);
    NUMERIC_CMP(>=)
    return;
}
void Not(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    switch (a.type){
        case ValueType::String: {
            auto item=getVariantOrThrow<std::string>(a);
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
            Value b=coerce(a,ValueType::Bool);
            bool item=getVariantOrThrow<bool>(b);
            ctx.stack.push_back(mkValue(!item));
            return;
    }
    return;
}
void And(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool v1=getVariantOrThrow<bool>(a1);
    bool v2=getVariantOrThrow<bool>(a2);
    bool ret=v1&&v2;
    ctx.stack.push_back(mkValue(ret));
    return;
}
void Or(const LinkedInstruction& instr, FNctx& ctx){
    Value a1=pop(ctx.stack);
    Value a2=pop(ctx.stack);
    bool v1=getVariantOrThrow<bool>(a1);
    bool v2=getVariantOrThrow<bool>(a2);
    bool ret=v1||v2;
    ctx.stack.push_back(mkValue(ret));
    return;
}
void Coerce(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    Value coerced=coerce(a,static_cast<ValueType>(instr.operand));
    ctx.stack.push_back(coerced);
    return;
}
void Typeof(const LinkedInstruction& instr, FNctx& ctx){
    Value a=pop(ctx.stack);
    const std::string& typestring=ValueTypeAsString.at(a.type);
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
    for (auto & i : fn.code) {
        if (static_cast<Opcode>(i.opcode)==Opcode::JumpLabel) {
            if (labelOffsets.contains(i.operand)) throw std::runtime_error("Duplicate label");
            labelOffsets.emplace(i.operand,newFn.size());
        } else {
            newFn.push_back(i);
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
        std::cout << "pc: " << ctx.pc << "\n";
        LinkedInstruction instr=func.function.code[ctx.pc];
        try {
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
        } catch (const ExecError& e){
            throw ExecutionError(id,e.message + " at instruction "+std::to_string(ctx.pc)+" of type "+opcodeEntryFor(static_cast<Opcode>(instr.opcode)).primary+", stack has "+std::to_string(ctx.stack.size())+" items");
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