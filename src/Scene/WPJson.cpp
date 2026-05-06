module;

#include <rstd/macro.hpp>
#include <nlohmann/json.hpp>
#include "Utils/String.h"

module wescene.json;
import cppstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;

namespace owe
{

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result) {
    try {
        result = nlohmann::json::parse(source);
    } catch (nlohmann::json::parse_error& e) {
        rstd_error("parse json({}), {} at {}:{}",
                   std::string_view(func), std::string_view(e.what()),
                   std::string_view(file), line);
        return false;
    }
    return true;
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json&                  json,
                          typename utils::is_std_array<T>::type& value) {
    using Tv          = typename T::value_type;
    const auto* pjson = &json;
    if (json.contains("value")) pjson = &json.at("value");
    const auto& njson = *pjson;
    if (njson.is_number()) {
        value = { njson.get<Tv>() };
        return true;
    } else {
        std::string strvalue;
        strvalue = njson.get<std::string>();
        return utils::StrToArray::Convert(strvalue, value);
    }
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, T& value) {
    if (json.contains("value"))
        value = json.at("value").get<T>();
    else
        value = json.get<T>();
    return true;
}

template<typename T>
inline bool _GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json,
                          T& value, bool warn, const char* name) {
    (void)warn;

    using njson = nlohmann::json;
    std::string nameinfo;
    if (name != nullptr) nameinfo = std::string("(key: ") + name + ")";
    try {
        return _GetJsonValue<T>(json, value);
    } catch (const njson::type_error& e) {
        rstd_info("{} {} at {} {}:{}\n{}",
                  std::string_view(e.what()), nameinfo,
                  std::string_view(func), std::string_view(file), line,
                  json.dump(4));
    } catch (const std::invalid_argument& e) {
        rstd_error("{} {} at {} {}:{}", std::string_view(e.what()), nameinfo,
                   std::string_view(func), std::string_view(file), line);
    } catch (const std::out_of_range& e) {
        rstd_error("{} {} at {} {}:{}", std::string_view(e.what()), nameinfo,
                   std::string_view(func), std::string_view(file), line);
    } catch (const utils::StrToArray::WrongSizeExp& e) {
        rstd_error("{} {} at {} {}:{}", std::string_view(e.what()), nameinfo,
                   std::string_view(func), std::string_view(file), line);
    }
    return false;
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name_view, bool warn) {
    std::string name { name_view };
    if (has_name) {
        if (! json.contains(name)) {
            if (warn)
                rstd_info("read json \"{}\" not a key at {}({}:{})",
                          name, std::string_view(func),
                          std::string_view(file), line);
            return false;
        } else if (json.at(name).is_null()) {
            if (warn)
                rstd_info("read json \"{}\" is null at {}({}:{})",
                          name, std::string_view(func),
                          std::string_view(file), line);
            return false;
        }
    }
    return _GetJsonValue<T>(file,
                            func,
                            line,
                            has_name ? json.at(name) : json,
                            value,
                            warn,
                            name.empty() ? nullptr : name.c_str());
}

#define T_IMPL_GET_JSON(TYPE)                                                            \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>(const char*,           \
                                                                  const char*,           \
                                                                  int,                   \
                                                                  const nlohmann::json&, \
                                                                  TYPE&,                 \
                                                                  bool,                  \
                                                                  std::string_view,      \
                                                                  bool);

T_IMPL_GET_JSON(bool);
T_IMPL_GET_JSON(int32_t);
T_IMPL_GET_JSON(uint32_t);
T_IMPL_GET_JSON(float);
T_IMPL_GET_JSON(double);
T_IMPL_GET_JSON(std::string);
T_IMPL_GET_JSON(std::vector<float>);
T_IMPL_GET_JSON(std::vector<int32_t>);

template<std::size_t N>
using iarray = std::array<int, N>;
T_IMPL_GET_JSON(iarray<3>);

template<std::size_t N>
using farray = std::array<float, N>;
T_IMPL_GET_JSON(farray<2>);
T_IMPL_GET_JSON(farray<3>);

} // namespace owe
