module;
#include <rstd/macro.hpp>

export module wescene.io;
import rstd;
import rstd.cppstd;

using ::alloc::vec::Vec;
using namespace rstd::prelude;

export namespace owe::io
{

enum class ByteOrder : u8
{
    BigEndian,
    LittleEndian,
};

template<typename T>
constexpr auto byte_swap(T value) -> T;

template<>
constexpr auto byte_swap<u64>(u64 value) -> u64 {
    return ((value & 0x00000000000000ffULL) << 56) | ((value & 0x000000000000ff00ULL) << 40) |
           ((value & 0x0000000000ff0000ULL) << 24) | ((value & 0x00000000ff000000ULL) << 8) |
           ((value & 0x000000ff00000000ULL) >> 8) | ((value & 0x0000ff0000000000ULL) >> 24) |
           ((value & 0x00ff000000000000ULL) >> 40) | ((value & 0xff00000000000000ULL) >> 56);
}

template<>
constexpr auto byte_swap<u32>(u32 value) -> u32 {
    return ((value & 0x000000ffU) << 24) | ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) | ((value & 0xff000000U) >> 24);
}

template<>
constexpr auto byte_swap<u16>(u16 value) -> u16 {
    return u16(((value & 0x00ffU) << 8) | ((value & 0xff00U) >> 8));
}

template<>
constexpr auto byte_swap<u8>(u8 value) -> u8 {
    return value;
}

template<>
constexpr auto byte_swap<i64>(i64 value) -> i64 {
    return i64(byte_swap(u64(value)));
}

template<>
constexpr auto byte_swap<i32>(i32 value) -> i32 {
    return i32(byte_swap(u32(value)));
}

template<>
constexpr auto byte_swap<i16>(i16 value) -> i16 {
    return i16(byte_swap(u16(value)));
}

template<>
constexpr auto byte_swap<i8>(i8 value) -> i8 {
    return value;
}

consteval auto system_byte_order() -> ByteOrder {
#ifdef WP_BIG_ENDIAN
    return ByteOrder::BigEndian;
#else
    return ByteOrder::LittleEndian;
#endif
}

class BinaryReader {
public:
    explicit BinaryReader(rstd::io::ReadRange range): BinaryReader(prepare(rstd::move(range))) {}

    explicit BinaryReader(Vec<u8> bytes): BinaryReader(prepare(rstd::move(bytes))) {}

    explicit BinaryReader(std::vector<u8>&& bytes): BinaryReader(prepare_std(rstd::move(bytes))) {}

    explicit BinaryReader(BinaryReader& source): BinaryReader(read_remaining(source)) {}

    BinaryReader(const BinaryReader&)                        = delete;
    auto operator=(const BinaryReader&) -> BinaryReader&     = delete;
    BinaryReader(BinaryReader&&) noexcept                    = default;
    auto operator=(BinaryReader&&) noexcept -> BinaryReader& = default;

    void set_byte_order(ByteOrder order) noexcept { m_byte_order = order; }
    void SetByteOrder(ByteOrder order) noexcept { set_byte_order(order); }

    auto read(void* buffer, usize size) -> rstd::io::Result<usize> {
        auto result = m_handle->read(static_cast<u8*>(buffer), size);
        if (result.is_ok()) m_position += *result;
        return result;
    }

    auto read_exact(void* buffer, usize size) -> rstd::io::Result<empty> {
        auto* bytes = static_cast<u8*>(buffer);
        while (size > 0) {
            auto count = rstd_try(read(bytes, size));
            if (count == 0) {
                return Err(rstd::io::error::Error::from_kind(
                    rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::UnexpectedEof }));
            }
            bytes += count;
            size -= count;
        }
        return Ok(empty {});
    }

    auto seek(rstd::io::SeekFrom from) -> rstd::io::Result<u64> {
        auto result = m_handle->seek(from);
        if (result.is_ok()) m_position = *result;
        return result;
    }

    auto position() const noexcept -> u64 { return m_position; }
    auto len() const noexcept -> u64 { return m_len; }
    auto remaining() const noexcept -> u64 { return m_position < m_len ? m_len - m_position : 0; }

    usize Read(void* buffer, usize size) {
        auto result = read(buffer, size);
        return result.is_ok() ? rstd::move(result).unwrap_unchecked() : 0;
    }

    char* Gets(char* buffer, usize size) {
        auto read = Read(buffer, size);
        return read == 0 ? nullptr : buffer;
    }

    idx Tell() const noexcept { return idx(m_position); }

    bool SeekSet(idx offset) {
        return offset >= 0 && seek(rstd::io::SeekFrom::from_start(u64(offset))).is_ok();
    }

    bool SeekCur(idx offset) { return seek(rstd::io::SeekFrom::from_current(i64(offset))).is_ok(); }

    bool SeekEnd(idx offset) { return seek(rstd::io::SeekFrom::from_end(i64(offset))).is_ok(); }

    isize Size() const noexcept { return isize(m_len); }
    usize Usize() const noexcept { return usize(m_len); }
    bool  Rewind() { return SeekSet(0); }

    f32 ReadFloat() {
        f32 value { 0.0F };
        Read(&value, sizeof(value));
        return value;
    }

    i64 ReadInt64() { return read_integer<i64>(); }
    u64 ReadUint64() { return read_integer<u64>(); }
    i32 ReadInt32() { return read_integer<i32>(); }
    u32 ReadUint32() { return read_integer<u32>(); }
    i16 ReadInt16() { return read_integer<i16>(); }
    u16 ReadUint16() { return read_integer<u16>(); }
    i8  ReadInt8() { return read_integer<i8>(); }
    u8  ReadUint8() { return read_integer<u8>(); }

    std::string ReadStr() {
        std::string value;
        char        current = 0;
        while (Read(&current, 1) == 1 && current != '\0') value.push_back(current);
        return value;
    }

    auto read_all_string() -> rstd::io::Result<std::string> {
        if (remaining() > u64(std::numeric_limits<usize>::max())) {
            return rstd::Err(rstd::io::error::Error::from_kind(
                rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::InvalidData }));
        }
        std::string value(usize(remaining()), '\0');
        rstd_try(read_exact(value.data(), value.size()));
        return Ok(rstd::move(value));
    }

    std::string ReadAllStr() {
        auto value = read_all_string();
        return value.is_ok() ? rstd::move(value).unwrap_unchecked() : std::string {};
    }

private:
    struct Prepared {
        rstd::io::ReadSeekHandle handle;
        u64                      len;
    };

    BinaryReader(rstd::io::ReadSeekHandle handle, u64 len)
        : m_handle(rstd::move(handle)), m_len(len) {}

    explicit BinaryReader(Prepared prepared)
        : BinaryReader(rstd::move(prepared.handle), prepared.len) {}

    static auto prepare(Vec<u8> bytes) -> Prepared {
        auto len    = u64(bytes.len());
        auto cursor = rstd::io::Cursor<Vec<u8>>(rstd::move(bytes));
        return Prepared { .handle = rstd::io::ReadSeekHandle::make(rstd::move(cursor)),
                          .len    = len };
    }

    static auto prepare_std(std::vector<u8>&& bytes) -> Prepared {
        auto data = Vec<u8>::with_capacity(bytes.size());
        for (auto value : bytes) data.push(u8(value));
        return prepare(rstd::move(data));
    }

    static auto read_remaining(BinaryReader& source) -> Vec<u8> {
        auto bytes = Vec<u8>::with_capacity(usize(source.remaining()));
        bytes.resize(usize(source.remaining()), u8(0));
        auto count = source.Read(bytes.data(), bytes.len());
        bytes.truncate(count);
        return bytes;
    }

    template<typename T>
    auto read_integer() -> T {
        T value { 0 };
        if (Read(&value, sizeof(value)) != sizeof(value)) return T { 0 };
        if (m_byte_order != owe::io::system_byte_order()) value = byte_swap(value);
        return value;
    }

    static auto prepare(rstd::io::ReadRange range) -> Prepared {
        auto len = range.len();
        return Prepared { .handle = rstd::io::ReadSeekHandle::make(rstd::move(range).into_reader()),
                          .len    = len };
    }

    rstd::io::ReadSeekHandle m_handle;
    u64                      m_len { 0 };
    u64                      m_position { 0 };
    ByteOrder                m_byte_order { ByteOrder::LittleEndian };
};

class BinaryWriter {
public:
    explicit BinaryWriter(rstd::io::WriteSeekHandle handle): m_handle(rstd::move(handle)) {}

    BinaryWriter(const BinaryWriter&)                        = delete;
    auto operator=(const BinaryWriter&) -> BinaryWriter&     = delete;
    BinaryWriter(BinaryWriter&&) noexcept                    = default;
    auto operator=(BinaryWriter&&) noexcept -> BinaryWriter& = default;

    void set_byte_order(ByteOrder order) noexcept { m_byte_order = order; }
    void SetByteOrder(ByteOrder order) noexcept { set_byte_order(order); }

    auto write(const void* buffer, usize size) -> rstd::io::Result<empty> {
        auto* bytes = static_cast<const u8*>(buffer);
        while (size > 0) {
            auto count = rstd_try(m_handle->write(bytes, size));
            if (count == 0) {
                return Err(rstd::io::error::Error::from_kind(
                    rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::WriteZero }));
            }
            bytes += count;
            size -= count;
        }
        return Ok(empty {});
    }

    usize Write(const void* buffer, usize size) { return write(buffer, size).is_ok() ? size : 0; }
    i32   WriteInt32(i32 value) { return write_integer(value) ? i32(sizeof(value)) : 0; }
    i32   WriteUint32(u32 value) { return write_integer(value) ? i32(sizeof(value)) : 0; }

    auto flush() -> rstd::io::Result<empty> { return m_handle->flush(); }

private:
    template<typename T>
    bool write_integer(T value) {
        if (m_byte_order != system_byte_order()) value = byte_swap(value);
        return write(&value, sizeof(value)).is_ok();
    }

    rstd::io::WriteSeekHandle m_handle;
    ByteOrder                 m_byte_order { ByteOrder::LittleEndian };
};

} // namespace owe::io
