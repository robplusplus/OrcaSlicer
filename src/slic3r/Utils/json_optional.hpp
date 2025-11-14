#pragma once

// Adapter to enable nlohmann::json <-> std::optional<T> conversions.
// Place this header where you need optional support and include it AFTER
// including <nlohmann/json.hpp>.

#include <optional>
#include "nlohmann/json.hpp"

// Note: If the bundled nlohmann::json already provides optional support,
// this specialization may clash. In practice, older versions don’t ship it;
// include this file only where needed to avoid multiple definition.

namespace nlohmann {

template <class T>
struct adl_serializer<std::optional<T>> {
    static void to_json(nlohmann::json &j, const std::optional<T> &opt)
    {
        if (opt.has_value())
            j = *opt;         // serialize contained value
        else
            j = nullptr;      // represent disengaged optional as null
    }

    static void from_json(const nlohmann::json &j, std::optional<T> &opt)
    {
        // Missing or explicit null becomes std::nullopt
        if (j.is_discarded() || j.is_null()) {
            opt = std::nullopt;
            return;
        }
        opt = j.get<T>();
    }
};

} // namespace nlohmann
