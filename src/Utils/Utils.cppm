module;

// Module purview content needs these std headers + a few internal classics.
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"
#include "Core/StringHelper.hpp"

// Stays-classic headers (macros forced classic, plus their classic-attached
// declarations they expose). GMF-included so we can re-export the few
// non-macro entities they declare via `using` decls below.
//
//  - Logging.h    : LOG_INFO/LOG_ERROR/__SHORT_FILE__ macros + classic-attached
//                   WallpaperLog / logToTmpfileWithSha1 / past_last_slash /
//                   LOGLEVEL_*. Macro-bound → must be #include'd directly.
//  - String.h     : STRTONUM macro + utils::StrToNum/SpliteString/StrToArray.
//                   Macro-bound → must be #include'd directly.
//  - AutoDeletor.hpp : AUTO_DELETER macro + wallpaper::AutoDeleter template.
//                      Macro-bound → must be #include'd directly.
//  - Sha.hpp      : utils::genSha1 + utils::SHA1_LEN. Stays classic because
//                   classic Logging.cpp's logToTmpfileWithSha1 calls genSha1
//                   (and Logging.cpp can't migrate — its WallpaperLog/
//                    logToTmpfileWithSha1 are declared classic in Logging.h).
#include "Utils/Logging.h"
#include "Utils/Sha.hpp"

export module wescene.utils;

// ---------- Module-purview entities ----------------------------------------

export namespace wallpaper
{

// BitFlags<EnumT> — was Utils/BitFlags.hpp.
template<typename EnumT>
class BitFlags {
    static_assert(std::is_enum_v<EnumT>, "Flags can only be specialized for enum types");

    using UnderlyingT = typename std::make_unsigned_t<typename std::underlying_type_t<EnumT>>;

public:
    constexpr BitFlags() noexcept: bits_(0u) {}
    constexpr BitFlags(UnderlyingT val) noexcept: bits_(val) {}

    BitFlags& set(EnumT e, bool value = true) noexcept {
        bits_.set(underlying(e), value);
        return *this;
    }
    BitFlags& reset(EnumT e) noexcept {
        set(e, false);
        return *this;
    }
    BitFlags& reset() noexcept {
        bits_.reset();
        return *this;
    }
    [[nodiscard]] bool        all() const noexcept { return bits_.all(); }
    [[nodiscard]] bool        any() const noexcept { return bits_.any(); }
    [[nodiscard]] bool        none() const noexcept { return bits_.none(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return bits_.size(); }
    [[nodiscard]] std::size_t count() const noexcept { return bits_.count(); }
    constexpr bool operator[](EnumT e) const { return bits_[underlying(e)]; }
    constexpr bool operator[](UnderlyingT t) const { return bits_[t]; }
    auto           to_string() const { return bits_.to_string(); }

private:
    static constexpr UnderlyingT underlying(EnumT e) { return static_cast<UnderlyingT>(e); }
    std::bitset<sizeof(UnderlyingT) * 8> bits_;
};

// FpsCounter — was Utils/FpsCounter.h. Implementation in Utils/FpsCounter.cpp.
class FpsCounter {
public:
    FpsCounter();
    u32  Fps() const { return m_fps; }
    u32  FrameCount() const { return m_frameCount; }
    void RegisterFrame();

private:
    u32                                                m_fps;
    u32                                                m_frameCount;
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
};

namespace algorism
{
// Was Utils/Algorism.h. Out-of-line in Utils/Algorism.cpp module impl unit.
double CalculatePersperctiveDistance(double fov, double height) noexcept;
double CalculatePersperctiveFov(double distence, double height) noexcept;
double PerlinNoise(double x, double y, double z) noexcept;

constexpr u32 PowOfTwo(u32 x) {
    u32 pow2 { 8 };
    while (pow2 < x) pow2 *= 2;
    return pow2;
}
constexpr bool IsPowOfTwo(u32 x) { return (x > 1) && ((x & (x - 1)) == 0); }

inline Eigen::Vector3d sph2cart(const Eigen::Vector3d& sph) noexcept {
    double azimuth   = sph.x();
    double elevation = sph.y();
    double radius    = sph.z();
    return radius * Eigen::Vector3d {
        std::cos(azimuth) * std::cos(elevation),
        std::sin(azimuth) * std::cos(elevation),
        std::sin(elevation),
    };
}

template<typename TFUNC>
Eigen::Vector3d GenSphereSurface(TFUNC&& random) noexcept {
    double azimuth   = 2.0 * EIGEN_PI * random();
    double elevation = std::asin(2.0 * random() - 1.0);
    return sph2cart({ azimuth, elevation, 1.0 });
}

template<typename TFUNC>
Eigen::Vector3d GenSphereSurfaceNormal(TFUNC&&                normal_random,
                                       const Eigen::Vector3d& direct) noexcept {
    double u    = direct.x() > 0.0 ? normal_random(0.0, direct.x()) : 0.0;
    double v    = direct.y() > 0.0 ? normal_random(0.0, direct.y()) : 0.0;
    double w    = direct.z() > 0.0 ? normal_random(0.0, direct.z()) : 0.0;
    double norm = std::sqrt((u * u + v * v + w * w));
    return Eigen::Vector3d(u, v, w) / norm;
}

template<typename TFUNC>
Eigen::Vector3d GenSphereIn(TFUNC&& random) noexcept {
    return std::pow(random(), 1.0 / 3.0) * GenSphereSurface(random);
}

constexpr double DragForce(double speed, double strength, double density) {
    return -2.0 * speed * strength * density;
}
inline Eigen::Vector3d DragForce(Eigen::Vector3d v, double strength,
                                 double density = 1.0) noexcept {
    return v.normalized() * DragForce(v.norm(), strength, density);
}

constexpr double lerp(double t, double a, double b) noexcept { return a + t * (b - a); }

constexpr double PerlinEase(double t) noexcept { return t * t * t * (t * (t * 6 - 15) + 10); }

inline Eigen::Vector3d PerlinNoiseVec3(Eigen::Vector3d p) noexcept {
    return Eigen::Vector3d { PerlinNoise(p[0], p[1], p[2]),
                             PerlinNoise(p[0] + 89.2, p[1] + 33.1, p[2] + 57.3),
                             PerlinNoise(p[0] + 100.3, p[1] + 120.1, p[2] + 142.2) };
}

inline Eigen::Vector3d CurlNoise(Eigen::Vector3d p) noexcept {
    using namespace Eigen;
    constexpr double e = 1e-5;
    Vector3d         dx(e, 0, 0), dy(0, e, 0), dz(0, 0, e);
    Vector3d x0 = PerlinNoiseVec3(p - dx), x1 = PerlinNoiseVec3(p + dx),
             y0 = PerlinNoiseVec3(p - dy), y1 = PerlinNoiseVec3(p + dy),
             z0 = PerlinNoiseVec3(p - dz), z1 = PerlinNoiseVec3(p + dz);
    double x = y1.z() - y0.z() - z1.y() + z0.y();
    double y = z1.x() - z0.x() - x1.z() + x0.z();
    double z = x1.y() - x0.y() - y1.x() + y0.x();
    return Vector3d(x, y, z) / (2.0 * e);
}
} // namespace algorism

namespace platform
{
// Was Utils/Platform.hpp.
inline std::filesystem::path GetCachePath(std::string_view name) {
    using namespace std::filesystem;

    path             p_cache;
    std::string_view home = sview_nullsafe(std::getenv("HOME"));
    if (! home.empty()) {
        std::string_view cache = sview_nullsafe(std::getenv("XDG_CACHE_HOME"));
        if (cache.empty())
            p_cache = path(home) / ".cache";
        else
            p_cache = path(cache);
    }
    return p_cache / name;
}
} // namespace platform

} // namespace wallpaper (export)

// ---------- Migrated from former Utils/{Hash,DynamicLibrary,Identity} -------

export namespace utils
{

// Re-exported from classic Sha.hpp (kept classic for classic-Logging.cpp).
using ::utils::genSha1;

// Was Utils/Hash.h. boost::functional/hash inline templates.
template<typename T>
inline void hash_combine(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
template<typename T>
inline void hash_combine_fast(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) << 1u;
}

// Was Utils/Identity.hpp. Used by wescene.json's _GetJsonValue<T> overloads.
template<typename>
struct is_std_array {};
template<typename T>
struct is_std_array<std::vector<T>> {
    using type = std::vector<T>;
};
template<typename T, std::size_t N>
struct is_std_array<std::array<T, N>> {
    using type = std::array<T, N>;
};

// Was Utils/DynamicLibrary.hpp. Definitions in Utils/DynamicLibrary.cpp.
class DynamicLibrary : NoCopy {
public:
    DynamicLibrary();
    ~DynamicLibrary();

    DynamicLibrary(const char* filename);

    DynamicLibrary(DynamicLibrary&& o) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& o) noexcept;

    bool IsOpen() const;
    bool Open(const char* filename);
    void Close();

    void* GetSymbolAddr(const char* name) const;

    // not using func deduction
    template<typename T>
    bool GetSymbol(const char* name, T& pfunc) const {
        pfunc = reinterpret_cast<T>(GetSymbolAddr(name));
        return pfunc != nullptr;
    }

private:
    void* handle { nullptr };
};

} // namespace utils (export)

// Eigen helpers — were Utils/Eigen.h. Module-attached additions to the Eigen
// namespace; only visible to TUs that `import wescene.utils;`.
export namespace Eigen
{
constexpr double Radians(double a) noexcept { return (a / 180.0f) * (double)EIGEN_PI; }

inline Matrix4d LookAt(Vector3d eye, Vector3d center, Vector3d up) noexcept {
    Vector3d camDir = center - eye;
    Vector3d zAxis  = -camDir.normalized();
    Vector3d xAxis  = up.cross(zAxis).normalized();
    Vector3d yAxis  = zAxis.cross(xAxis).normalized();

    Affine3d trans         = Affine3d::Identity();
    trans.linear().col(0)  = xAxis;
    trans.linear().col(1)  = yAxis;
    trans.linear().col(2)  = zAxis;
    trans                 *= Translation3d(-eye);
    return trans.matrix();
}

inline Matrix4d Ortho(double left, double right, double bottom, double top, double nearz,
                      double farz) noexcept {
    Affine3d trans = Affine3d::Identity();
    trans.pretranslate(
        Vector3d(-(left + right) / 2.0f, -(top + bottom) / 2.0f, -(nearz + farz) / 2.0f));
    trans.prescale(Vector3d(2.0f / (right - left), 2.0f / (top - bottom), 2.0f / (farz - nearz)));
    trans.scale(Vector3d(1.0f, 1.0f, -1.0f));
    trans.prescale(Vector3d(1.0f, 1.0f, 0.5f));
    trans.pretranslate(Vector3d(0.0f, 0.0f, 0.5f));
    return trans.matrix();
}

inline Matrix4d Perspective(double fov, double aspect, double nearz, double farz) noexcept {
    Projective3d trans = Projective3d::Identity();
    trans.prescale(Vector3d(nearz, nearz, (nearz + farz)));
    trans(3, 2)  = 1.0f;
    trans(3, 3)  = 0.0f;
    trans(2, 3)  = -nearz * farz;
    double top   = std::tan(fov / 2.0f) * std::abs(nearz);
    double right = top * aspect;
    trans.scale(Vector3d(1.0f, 1.0f, -1.0f));
    trans.prescale(Vector3d(1.0f, 1.0f, -1.0f));
    return Ortho(-right, right, -top, top, nearz, farz) * trans.matrix();
}
} // namespace Eigen (export)
