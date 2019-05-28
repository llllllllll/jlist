#pragma once
#include <optional>
#include <utility>

namespace jl {
template<typename F>
struct scope_guard {
private:
    std::optional<F> m_callback;

public:
    scope_guard(F&& callback) : m_callback(std::move(callback)) {}

    void dismiss() {
        m_callback = std::nullopt;
    }

    ~scope_guard() {
        if (m_callback) {
            (*m_callback)();
        }
    }
};
}  // namespace jl
