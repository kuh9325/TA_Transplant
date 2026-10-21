#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rwe
{
    /**
     * Native saved-game binary format.
     *
     * Retail TotalA.exe writes savegame\<name>.sav using an internal tagged
     * key-value serializer over raw engine structures; reproducing that byte
     * layout would require reverse-engineering undocumented internals, so RWE
     * uses its own versioned container instead (see DECISIONS.md). The file
     * keeps the retail directory/name contract (savegame/<name>.sav) but the
     * payload is not retail-compatible.
     *
     * File container layout (all integers little-endian):
     *   offset 0  : 8-byte magic "RWESAVE\x00"
     *   offset 8  : u32 formatVersion (currently 1)
     *   offset 12 : u32 payloadSize
     *   offset 16 : u32 fnv1a-32 checksum of the payload
     *   offset 20 : payload bytes (SaveWriter-encoded document)
     */

    class SaveFormatException : public std::runtime_error
    {
    public:
        explicit SaveFormatException(const std::string& message) : std::runtime_error(message) {}
    };

    class SaveWriter
    {
    private:
        std::vector<char> data;

    public:
        void u8(std::uint8_t v);
        void u16(std::uint16_t v);
        void u32(std::uint32_t v);
        void u64(std::uint64_t v);
        void i32(std::int32_t v);
        void boolean(bool v);

        /** Raw bytes with an explicit u32 length prefix. */
        void bytes(const char* ptr, std::size_t size);
        void bytes(const std::vector<char>& v);
        void bytes(const std::vector<unsigned char>& v);

        /** Length-prefixed UTF-8 string (u32 length + chars, no terminator). */
        void string(const std::string& v);

        /** Fixed-width field with no length prefix -- caller must match on read. */
        void raw(const void* ptr, std::size_t size);

        /** Float/double stored as their exact bit patterns. */
        void f32bits(float v);
        void f64bits(double v);

        const std::vector<char>& getData() const { return data; }
        std::size_t size() const { return data.size(); }
    };

    class SaveReader
    {
    private:
        const char* data;
        std::size_t dataSize;
        std::size_t pos{0};

    public:
        explicit SaveReader(const std::vector<char>& data) : data(data.data()), dataSize(data.size()) {}
        SaveReader(const char* data, std::size_t size) : data(data), dataSize(size) {}

        std::uint8_t u8();
        std::uint16_t u16();
        std::uint32_t u32();
        std::uint64_t u64();
        std::int32_t i32();
        bool boolean();

        std::vector<char> bytes();
        std::vector<unsigned char> ubytes();
        std::string string();

        void raw(void* out, std::size_t size);

        float f32bits();
        double f64bits();

        bool atEnd() const { return pos == dataSize; }
        std::size_t remaining() const { return dataSize - pos; }
        std::size_t position() const { return pos; }

    private:
        void require(std::size_t n, const char* what);
    };

    /**
     * Wrap a serialized payload in the RWESAVE container.
     * The returned buffer is the exact file content to write.
     */
    std::vector<char> encodeSaveFile(std::uint32_t formatVersion, const std::vector<char>& payload);

    /**
     * Unwrap a RWESAVE container. Throws SaveFormatException on bad magic,
     * unsupported version, truncated data, or checksum mismatch.
     */
    std::vector<char> decodeSaveFile(const std::vector<char>& fileBytes, std::uint32_t& formatVersionOut);

    /**
     * Peek at the container header without validating the payload checksum.
     * Used by the save-list UI to detect non-RWE files quickly.
     */
    bool isRweSaveFile(const std::vector<char>& fileBytes);
}
