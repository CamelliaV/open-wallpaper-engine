#pragma once
// Macros only. The owe::GetJsonValue / owe::ParseJson templates
// are exported by the `wescene.json` module — every TU that uses these
// macros must also `import wescene.json;` (and #include <nlohmann/json.hpp>
// so the json type is in scope).
#include "Utils/Logging.h"  // for __SHORT_FILE__

#define GET_JSON_VALUE(json, value) \
    owe::GetJsonValue(        \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", true)
#define GET_JSON_NAME_VALUE(json, name, value) \
    owe::GetJsonValue(                   \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), true)

#define GET_JSON_VALUE_NOWARN(json, value) \
    owe::GetJsonValue(               \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", false)
#define GET_JSON_NAME_VALUE_NOWARN(json, name, value) \
    owe::GetJsonValue(                          \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), false)

#define PARSE_JSON(source, result) \
    owe::ParseJson(__SHORT_FILE__, __FUNCTION__, __LINE__, (source), (result))
