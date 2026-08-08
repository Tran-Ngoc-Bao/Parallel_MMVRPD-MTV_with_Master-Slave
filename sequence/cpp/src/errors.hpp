#pragma once
#include <stdexcept>
#include <optional>
#include <string>

template<typename T>
T expected_value(std::optional<T> val, const std::string& msg = "Unexpected nullopt") {
    if (!val.has_value()) throw std::runtime_error(msg);
    return std::move(*val);
}
