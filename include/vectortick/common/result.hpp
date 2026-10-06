#pragma once

#include "status.hpp"
#include <utility>
#include <variant>
#include <optional>

namespace vectortick {

// Result type that holds either a value or an error
// This is similar to Rust's Result<T, E>
template <typename T>
class [[nodiscard]] Result {
public:
    // Construct from value
    Result(T value) : data_(std::move(value)) {}  // NOLINT: allow implicit conversion
    
    // Construct from Status (error case)
    Result(Status status) : data_(std::move(status)) {  // NOLINT: allow implicit conversion
        // Status must be an error
        if (status.ok()) {
            data_ = T{};  // Default construct if given OK status
        }
    }
    
    // Check if this contains a value
    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    
    // Check if this contains an error
    [[nodiscard]] bool has_error() const noexcept {
        return !ok();
    }
    
    // Get the value (undefined behavior if error)
    [[nodiscard]] T& value() & noexcept {
        return std::get<T>(data_);
    }
    
    [[nodiscard]] const T& value() const& noexcept {
        return std::get<T>(data_);
    }
    
    [[nodiscard]] T&& value() && noexcept {
        return std::get<T>(std::move(data_));
    }
    
    // Get the status (undefined behavior if ok)
    [[nodiscard]] const Status& status() const noexcept {
        return std::get<Status>(data_);
    }
    
    // Get value or default
    [[nodiscard]] T value_or(T default_value) const noexcept {
        return ok() ? value() : std::move(default_value);
    }
    
    // Conversion to bool (true if ok)
    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }
    
    // Dereference operators (undefined behavior if error)
    [[nodiscard]] T& operator*() & noexcept {
        return value();
    }
    
    [[nodiscard]] const T& operator*() const& noexcept {
        return value();
    }
    
    [[nodiscard]] T* operator->() noexcept {
        return &value();
    }
    
    [[nodiscard]] const T* operator->() const noexcept {
        return &value();
    }
    
    // Map the value if present
    template <typename F>
    [[nodiscard]] auto map(F&& f) const -> Result<std::invoke_result_t<F, const T&>> {
        using ResultType = std::invoke_result_t<F, const T&>;
        if (ok()) {
            return Result<ResultType>(f(value()));
        }
        return Result<ResultType>(status());
    }

private:
    std::variant<T, Status> data_;
};

// Specialization for void (just status)
template <>
class [[nodiscard]] Result<void> {
public:
    // Construct from success
    Result() : status_(StatusCode::OK) {}
    
    // Construct from status
    Result(Status status) : status_(std::move(status)) {}  // NOLINT: allow implicit conversion
    
    // Check if OK
    [[nodiscard]] bool ok() const noexcept {
        return status_.ok();
    }
    
    // Get the status
    [[nodiscard]] const Status& status() const noexcept {
        return status_;
    }
    
    // Conversion to bool (true if OK)
    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }

private:
    Status status_;
};

// Helper to create Results
template <typename T>
Result<T> make_result(T value) {
    return Result<T>(std::move(value));
}

inline Result<void> make_result() {
    return Result<void>();
}

template <typename T>
Result<T> make_error(StatusCode code, std::string_view msg = "") {
    return Result<T>(Status(code, msg));
}

// Macro for checking result and returning on error
#define VT_ASSIGN_OR_RETURN(var, expr) \
    auto _result_##var = (expr); \
    if (!_result_##var.ok()) { \
        return _result_##var.status(); \
    } \
    var = std::move(_result_##var.value())

} // namespace vectortick
