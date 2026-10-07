#pragma once

#include <functional>
#include <string>
#include <vector>

namespace console {

struct Cvar {
    std::string name;
    std::string description;
    float value;
    float minValue;
    float maxValue;
    std::function<void(float)> onChange;
    float defaultValue = 0.0f;
};

// named, live settable float values (sensitivity, sv_maxspeed etc), float only for v1 which covers every cvar actually needed so far. source's convars are string backed, if a string cvar is ever needed extend rather than widen this one
class CvarRegistry {
public:
    void add(const std::string& name, const std::string& description, float defaultValue,
              float minValue, float maxValue, std::function<void(float)> onChange = nullptr);

    bool has(const std::string& name) const;
    bool set(const std::string& name, float value);
    float get(const std::string& name) const;

    std::vector<Cvar>& all() { return cvars_; }
    const std::vector<Cvar>& all() const { return cvars_; }

private:
    std::vector<Cvar> cvars_;
};

}  // namespace console
