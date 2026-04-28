#pragma once
#include <charconv>
#include <string>
#include <system_error>
#include <vector>

namespace MMM::Internal
{

inline std::string safeAt(const std::vector<std::string>& v, size_t idx,
                          const std::string& defaultVal = "")
{
    if ( idx >= v.size() ) return defaultVal;
    return v[idx];
}

inline int safeStoi(const std::string& s, int defaultVal = 0)
{
    if ( s.empty() ) return defaultVal;
    try {
        return std::stoi(s);
    } catch ( ... ) {
        try {
            return static_cast<int>(std::stod(s));
        } catch ( ... ) {
            return defaultVal;
        }
    }
}

inline double safeStod(const std::string& s, double defaultVal = 0.0)
{
    if ( s.empty() ) return defaultVal;
    try {
        return std::stod(s);
    } catch ( ... ) {
        return defaultVal;
    }
}

}  // namespace MMM::Internal
