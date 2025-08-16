/// @date 12/02/2022 11:05:42
/// @author Ambroise Leclerc
/// @brief Type erased functions for uniform storable functions
#pragma once
#include <functional>
#include <memory>
#include <stdexcept>

#include <array>
#include <cstddef>
#include <iostream>

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

namespace impl {
template<typename T>
constexpr const auto& RawTypeName() {
#ifdef _MSC_VER
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

struct RawTypeNameFormat {
    std::size_t leading_junk = 0, trailing_junk = 0;
};

// Returns `false` on failure.
inline constexpr bool GetRawTypeNameFormat(RawTypeNameFormat* format) {
    const auto& str = RawTypeName<int>();
    for (std::size_t i = 0;; i++) {
        if (str[i] == 'i' && str[i + 1] == 'n' && str[i + 2] == 't') {
            if (format) {
                format->leading_junk = i;
                format->trailing_junk = sizeof(str) - i - 3 - 1; // `3` is the length of "int", `1` is the space for the null terminator.
            }
            return true;
        }
    }
    return false;
}

inline static constexpr RawTypeNameFormat format = [] {
    static_assert(GetRawTypeNameFormat(nullptr), "Unable to figure out how to generate type names on this compiler.");
    RawTypeNameFormat rawFormat;
    GetRawTypeNameFormat(&rawFormat);
    return rawFormat;
}();
} // namespace impl

// Returns the type name in a `std::array<char, N>` (null-terminated).
template<typename T>
[[nodiscard]] constexpr auto CexprTypeName() {
    constexpr std::size_t len = sizeof(impl::RawTypeName<T>()) - impl::format.leading_junk - impl::format.trailing_junk;
    std::array<char, len> name{};
    for (std::size_t i = 0; i < len - 1; i++) name[i] = impl::RawTypeName<T>()[i + impl::format.leading_junk];
    return name;
}

template<typename T>
[[nodiscard]] const char* TypeName() {
    static constexpr auto name = CexprTypeName<T>();
    return name.data();
}
template<typename T>
[[nodiscard]] const char* TypeName(const T&) {
    return TypeName<T>();
}

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
        : function(new FunctionPointer<typename function_traits<T>::type>(t)), voidFunction(new FunctionPointer<typename function_traits<T>::void_type>(t)) {
            std::cout << "TypeErasedFunction constructor()\n";
        }

    template<typename T, typename... Args>
    TypeErasedFunction(T&& t, Args&&... args)
        : function(new FunctionPointer<typename function_traits<T>::type>(std::bind(std::forward<T>(t), std::forward<Args>(args)...))),
          voidFunction(new FunctionPointer<typename function_traits<T>::void_type>(std::bind(std::forward<T>(t), std::forward<Args>(args)...))) {
        std::cout << "TypeErasedFunction constructor(";
        ((std::cout << TypeName(args)), ...);
        std::cout << ")\n";
    }

    void operator()() { this->operator()<>(); }

    template<typename... Args>
    void operator()(Args&&... args) {
        std::cout << "TypeErasedFunction operator(";
        ((std::cout << TypeName(args)), ...);
        std::cout << ")\n";
        auto f = dynamic_cast<FunctionPointer<std::function<void(Args...)>>*>(voidFunction.get());
        if (f) {
            f->call(std::forward<Args>(args)...);
            return;
        }
        throw std::invalid_argument("Wrong argument type");
    }

    template<typename R, typename... Args>
    R call(Args&&... args) {
        auto f = dynamic_cast<FunctionPointer<std::function<R(Args...)>>*>(function.get());
        if (f) { return f->template callRet<R>(std::forward<Args>(args)...); }
        throw std::invalid_argument("Wrong argument type");
    }

private:
    std::unique_ptr<TypeErased> function, voidFunction;
};

} // namespace webfront::utils