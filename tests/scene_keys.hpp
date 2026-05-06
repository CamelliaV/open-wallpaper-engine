// scene.json key-path scanner.
//
// Walks every workshop/<id>/scene.pkg under workshop_root, extracts the
// raw scene.json from the pkg, and aggregates per-pkg-version statistics
// of every key path observed in the json tree. Sister tool to wpdump:
// where dump.cpp produces structured field snapshots, this captures the
// raw key shape so unknown fields surface even when no parser touches
// them yet.

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace owe::testing {

// Returns:
//   {
//     "<pkg_version>": {
//       "total_scenes": <int>,
//       "keys": {
//         "<dot.path[].with.brackets>": {
//           "present_in":  <int>,   // # of scenes containing this path at least once
//           "occurrences": <int>,   // total absolute hits across all scenes
//           "value_types": ["object", "array", "string", ...]
//         },
//         ...
//       }
//     },
//     ...
//   }
//
// Per-workshop failures (missing scene.pkg, malformed header, missing or
// unparseable scene.json) are logged to stderr and skipped; the scan
// keeps going.
nlohmann::json ScanSceneKeys(const std::string& workshop_root);

} // namespace owe::testing
