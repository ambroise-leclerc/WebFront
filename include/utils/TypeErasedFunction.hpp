/// @date 12/02/2022 11:05:42
/// @author Ambroise Leclerc
/// @brief Type erased functions for uniform storable functions
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>

namespace webfront::utils {

template<typename R>
struct function_traits : public function_traits<decltype(&R::operator())> {};

template<typename R, typename... Args>
struct function_traits<R (*)(Args...)> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

template<typename R, typename... Args>
struct function_traits<std::function<R(Args...)>> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

template<typename R, typename ClassType, typename... Args>
struct function_traits<R (ClassType::*)(Args...) const> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

template<typename R, typename ClassType, typename... Args>
struct function_traits<R (ClassType::*)(Args...)> {
    using result_type = R;
    using type = typename std::function<R(Args...)>;
    using void_type = typename std::function<void(Args...)>;
};

class TypeErasedFunction {
private:
    struct TypeErased {
        TypeErased() {}
        virtual ~TypeErased() {}
    };

    template<typename T>
    struct FunctionPointer : TypeErased {
        FunctionPointer(T arg) : functionPointer(arg) {}

        template<typename... Args>
        void call(Args&&... args) {
            functionPointer(std::forward<Args>(args)...);
        }

        template<typename R, typename... Args>
        R callRet(Args&&... args) {
            return functionPointer(std::forward<Args>(args)...);
        }

        T functionPointer;
    };

public:
    template<typename T>
    TypeErasedFunction(T t)
        : function(new FunctionPointer<typename function_traits<T>::type>(t)), voidFunction(new FunctionPointer<typename function_traits<T>::void_type>(t)) {}

    template<typename T, typename... Args>
    TypeErasedFunction(T&& t, Args&&... args)
        : function(new FunctionPointer<typename function_traits<T>::type>(std::bind(std::forward<T>(t), std::forward<Args>(args)...))),
          voidFunction(new FunctionPointer<typename function_traits<T>::void_type>(std::bind(std::forward<T>(t), std::forward<Args>(args)...))) {}

    void operator()() { this->operator()<>(); }

    template<typename... Args>
    void operator()(Args&&... args) {
        auto f = dynamic_cast<FunctionPointer<std::function<void(Args...)>>*>(voidFunction.get());
        if (f) {
            f->call(std::forward<Args>(args)...);
            return;
        }
        throw std::invalid_argument("Wrong argument type.");
    }

    template<typename R, typename... Args>
    R call(Args&&... args) {
        auto f = dynamic_cast<FunctionPointer<std::function<R(Args...)>>*>(function.get());
        if (f) { return f->template callRet<R>(std::forward<Args>(args)...); }
        throw std::invalid_argument("");
    }

private:
    std::unique_ptr<TypeErased> function, voidFunction;
};

} // namespace webfront::utils