#include "SaveCodec.h"

#include <cstring>

namespace rwe
{
    namespace
    {
        constexpr char SaveFileMagic[8] = {'R', 'W', 'E', 'S', 'A', 'V', 'E', '\0'};
        constexpr std::size_t SaveFileHeaderSize = 8 + 4 + 4 + 4;

        std::uint32_t fnv1a32(const char* data, std::size_t size)
        {
            std::uint32_t hash = 2166136261u;
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= static_cast<unsigned char>(data[i]);
                hash *= 16777619u;
            }
            return hash;
        }
    }

    void SaveWriter::u8(std::uint8_t v)
    {
        data.push_back(static_cast<char>(v));
    }

    void SaveWriter::u16(std::uint16_t v)
    {
        data.push_back(static_cast<char>(v & 0xFFu));
        data.push_back(static_cast<char>((v >> 8u) & 0xFFu));
    }

    void SaveWriter::u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            data.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
        }
    }

    void SaveWriter::u64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
        {
            data.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
        }
    }

    void SaveWriter::i32(std::int32_t v)
    {
        u32(static_cast<std::uint32_t>(v));
    }

    void SaveWriter::boolean(bool v)
    {
        u8(v ? 1 : 0);
    }

    void SaveWriter::bytes(const char* ptr, std::size_t size)
    {
        u32(static_cast<std::uint32_t>(size));
        data.insert(data.end(), ptr, ptr + size);
    }

    void SaveWriter::bytes(const std::vector<char>& v)
    {
        bytes(v.data(), v.size());
    }

    void SaveWriter::bytes(const std::vector<unsigned char>& v)
    {
        bytes(reinterpret_cast<const char*>(v.data()), v.size());
    }

    void SaveWriter::string(const std::string& v)
    {
        bytes(v.data(), v.size());
    }

    void SaveWriter::raw(const void* ptr, std::size_t size)
    {
        const auto* p = static_cast<const char*>(ptr);
        data.insert(data.end(), p, p + size);
    }

    void SaveWriter::f32bits(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }

    void SaveWriter::f64bits(double v)
    {
        std::uint64_t bits;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }

    void SaveReader::require(std::size_t n, const char* what)
    {
        if (pos + n > dataSize)
        {
            throw SaveFormatException(std::string("unexpected end of save data while reading ") + what);
        }
    }

    std::uint8_t SaveReader::u8()
    {
        require(1, "u8");
        return static_cast<std::uint8_t>(data[pos++]);
    }

    std::uint16_t SaveReader::u16()
    {
        require(2, "u16");
        std::uint16_t v = static_cast<std::uint8_t>(data[pos]) | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[pos + 1])) << 8u);
        pos += 2;
        return v;
    }

    std::uint32_t SaveReader::u32()
    {
        require(4, "u32");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + i])) << (i * 8);
        }
        pos += 4;
        return v;
    }

    std::uint64_t SaveReader::u64()
    {
        require(8, "u64");
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[pos + i])) << (i * 8);
        }
        pos += 8;
        return v;
    }

    std::int32_t SaveReader::i32()
    {
        return static_cast<std::int32_t>(u32());
    }

    bool SaveReader::boolean()
    {
        auto v = u8();
        if (v > 1)
        {
            throw SaveFormatException("invalid boolean value in save data");
        }
        return v != 0;
    }

    std::vector<char> SaveReader::bytes()
    {
        auto n = u32();
        require(n, "byte buffer");
        std::vector<char> v(data + pos, data + pos + n);
        pos += n;
        return v;
    }

    std::vector<unsigned char> SaveReader::ubytes()
    {
        auto n = u32();
        require(n, "byte buffer");
        std::vector<unsigned char> v(data + pos, data + pos + n);
        pos += n;
        return v;
    }

    std::string SaveReader::string()
    {
        auto n = u32();
        require(n, "string");
        std::string v(data + pos, n);
        pos += n;
        return v;
    }

    void SaveReader::raw(void* out, std::size_t size)
    {
        require(size, "raw field");
        std::memcpy(out, data + pos, size);
        pos += size;
    }

    float SaveReader::f32bits()
    {
        auto bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    double SaveReader::f64bits()
    {
        auto bits = u64();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    std::vector<char> encodeSaveFile(std::uint32_t formatVersion, const std::vector<char>& payload)
    {
        SaveWriter w;
        w.raw(SaveFileMagic, sizeof(SaveFileMagic));
        w.u32(formatVersion);
        w.u32(static_cast<std::uint32_t>(payload.size()));
        w.u32(fnv1a32(payload.data(), payload.size()));
        auto out = w.getData();
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    std::vector<char> decodeSaveFile(const std::vector<char>& fileBytes, std::uint32_t& formatVersionOut)
    {
        if (fileBytes.size() < SaveFileHeaderSize)
        {
            throw SaveFormatException("file too small to be a save game");
        }
        if (std::memcmp(fileBytes.data(), SaveFileMagic, sizeof(SaveFileMagic)) != 0)
        {
            throw SaveFormatException("not an RWE save game (bad magic)");
        }
        SaveReader r(fileBytes.data() + 8, fileBytes.size() - 8);
        formatVersionOut = r.u32();
        auto payloadSize = r.u32();
        auto checksum = r.u32();
        if (payloadSize != r.remaining())
        {
            throw SaveFormatException("save game payload length mismatch");
        }
        std::vector<char> payload(fileBytes.begin() + SaveFileHeaderSize, fileBytes.end());
        if (fnv1a32(payload.data(), payload.size()) != checksum)
        {
            throw SaveFormatException("save game checksum mismatch (corrupt file)");
        }
        return payload;
    }

    bool isRweSaveFile(const std::vector<char>& fileBytes)
    {
        return fileBytes.size() >= SaveFileHeaderSize
            && std::memcmp(fileBytes.data(), SaveFileMagic, sizeof(SaveFileMagic)) == 0;
    }
}
