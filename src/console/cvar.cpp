#include "console/cvar.h"

namespace console {

void CvarRegistry::add(const std::string& name, const std::string& description, float defaultValue,
                         float minValue, float maxValue, std::function<void(float)> onChange) {
    cvars_.push_back({name, description, defaultValue, minValue, maxValue, std::move(onChange), defaultValue});
}

bool CvarRegistry::has(const std::string& name) const {
    for (const auto& cvar : cvars_) {
        if (cvar.name == name) {
            return true;
        }
    }
    return false;
}

bool CvarRegistry::set(const std::string& name, float value) {
    for (auto& cvar : cvars_) {
        if (cvar.name == name) {
            cvar.value = value;
            if (cvar.onChange) {
                cvar.onChange(value);
            }
            return true;
        }
    }
    return false;
}

float CvarRegistry::get(const std::string& name) const {
    for (const auto& cvar : cvars_) {
        if (cvar.name == name) {
            return cvar.value;
        }
    }
    return 0.0f;
}

}  // namespace console
