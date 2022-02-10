/// @date 09/02/2022 12:23:27
/// @author Ambroise Leclerc
/// @brief A type erased generalized function holder wich can store parameters types
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>

namespace webfront {

template <typename R>
struct function_traits : public function_traits<decltype(&R::operator())> {};

template<typename R, typename ClassType, typename ...Args> 
struct function_traits<R(ClassType::*)(Args...) const> {
    using result_type = R;
    using type =  typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

template<typename R, typename ClassType, typename ...Args> 
struct function_traits<R(ClassType::*)(Args...)> {
    using result_type = R;
    using type =  typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};


template<typename R, typename ...Args> 
struct function_traits<std::function<R(Args...)>> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

template<typename R, typename ...Args> 
struct function_traits<R(*)(Args...)> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};


class CppFunction {
private:
    struct TypeErase {
        TypeErase() {}
        virtual ~TypeErase() {}
    };

    template <typename T>
    struct FunctionPointer : TypeErase {
        FunctionPointer(T arg) : functionPointer(arg) {}

        template<typename... Args>
        void call(Args&&...args) { functionPointer(std::forward<Args>(args)...); }

        template<typename R, typename... Args>
        R callRet(Args&&...args) { return functionPointer(std::forward<Args>(args)...); }

        T functionPointer;
    };
    
private:
    enum class SupportedType { Undefined, String, ZString, CharArray, Int8, Int16, Int32, UInt8, UInt16, UInt32 };
    std::array<SupportedType, 32> argType;
    size_t argTypeIndex = 0;
    static constexpr std::string_view toString(SupportedType t) {
        using enum SupportedType;
        return (t == Undefined) ? "Undefined" : (t == String) ? "String" : (t == ZString) ? "ZString" : (t == CharArray) ? "CharArray"
        : (t == Int8) ? "Int8" : (t == Int16) ? "Int16" : (t == Int32) ? "Int32" : (t == UInt8) ? "UInt8" : "Error";
    }
    
    template<typename T>
    static constexpr SupportedType getType() {
        using enum SupportedType;
        if constexpr (std::is_same_v<T, std::string>) return String;
        if constexpr (std::is_same_v<T, uint8_t>) return UInt8;
    }
    
    template <typename Ret, typename... Args> 
    void decodeArgsTypes(std::function<Ret(Args...)>&&) { ((argType[argTypeIndex++] = getType<Args>()), ...); }

    template<typename R, typename... Args> 
    void decodeArgsTypes(R(*)(Args...)) { ((argType[argTypeIndex++] = getType<Args>()), ...); }
    
    template <typename Ret, typename... Args> 
    void innerDecodeArgsTypes(std::function<Ret(Args...)>&&) { ((argType[argTypeIndex++] = getType<Args>()), ...); }

    template<typename T>
    void decodeArgsTypes(auto&& t) {
        using trait = function_traits<decltype(t)>;
        innerDecodeArgsTypes<typename trait::type>(t);
    }
    
    

public:
    template<typename T>
    CppFunction(T t) : function(new FunctionPointer<typename function_traits<T>::type>(t))
                        , voidFunction(new FunctionPointer<typename function_traits<T>::void_type>(t)) {}

    template<typename T, typename...Args>
    CppFunction(T&& t, Args&&... args) : function(new FunctionPointer<typename function_traits<T>::type>
                                                (std::bind(std::forward<T>(t), std::forward<Args>(args)...)))
                                          , voidFunction(new FunctionPointer<typename function_traits<T>::void_type>
                                                (std::bind(std::forward<T>(t), std::forward<Args>(args)...))) {}

    void operator()() {
        this->operator()<>();
    }
    
    template<typename... Args>
    void operator()(Args&&... args) {
        auto f = dynamic_cast<FunctionPointer<std::function < void(Args...) > >*>(voidFunction.get());
        if (f) {
            f->call(std::forward<Args>(args)...);
            return;
        }
        throw std::invalid_argument("Wrong argument type.");
    }
    
    template<typename R, typename... Args>
    R call(Args&&... args) {
        auto f = dynamic_cast<FunctionPointer<std::function<R(Args...)>>*>(function.get());
        if (f) {
            return f->template callRet<R>(std::forward<Args>(args)...);
        }
        throw std::invalid_argument("");
    }

private:
    std::unique_ptr<TypeErase> function, voidFunction;
    std::string params;
};


} // namespace webfront
