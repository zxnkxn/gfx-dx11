#include "gltf_loader.h"

#include <directxmath.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <variant>

using namespace DirectX;

namespace
{
    namespace fs = std::filesystem;
    thread_local std::string g_lastErrorMessage;

    struct LoaderException : std::runtime_error
    {
        LoaderException(HRESULT value, const std::string& message) :
            std::runtime_error(message),
            hr(value)
        {
        }

        HRESULT hr;
    };

    [[noreturn]] void Throw(HRESULT hr, const std::string& message)
    {
        throw LoaderException(hr, message);
    }

    std::wstring ToWide(std::string_view text)
    {
        return std::wstring(text.begin(), text.end());
    }

    void Require(bool condition, HRESULT hr, const std::string& message)
    {
        if (!condition)
        {
            Throw(hr, message);
        }
    }

    struct JsonValue;

    struct JsonArray
    {
        std::vector<JsonValue> values;
    };

    struct JsonObject
    {
        std::map<std::string, JsonValue, std::less<>> values;
    };

    struct JsonValue
    {
        using Storage = std::variant<
            std::nullptr_t,
            bool,
            double,
            std::string,
            std::shared_ptr<JsonArray>,
            std::shared_ptr<JsonObject>>;

        JsonValue() :
            storage(nullptr)
        {
        }

        explicit JsonValue(std::nullptr_t value) :
            storage(value)
        {
        }

        explicit JsonValue(bool value) :
            storage(value)
        {
        }

        explicit JsonValue(double value) :
            storage(value)
        {
        }

        explicit JsonValue(std::string value) :
            storage(std::move(value))
        {
        }

        explicit JsonValue(std::shared_ptr<JsonArray> value) :
            storage(std::move(value))
        {
        }

        explicit JsonValue(std::shared_ptr<JsonObject> value) :
            storage(std::move(value))
        {
        }

        bool IsNull() const
        {
            return std::holds_alternative<std::nullptr_t>(storage);
        }

        bool IsBool() const
        {
            return std::holds_alternative<bool>(storage);
        }

        bool IsNumber() const
        {
            return std::holds_alternative<double>(storage);
        }

        bool IsString() const
        {
            return std::holds_alternative<std::string>(storage);
        }

        bool IsArray() const
        {
            return std::holds_alternative<std::shared_ptr<JsonArray>>(storage);
        }

        bool IsObject() const
        {
            return std::holds_alternative<std::shared_ptr<JsonObject>>(storage);
        }

        bool AsBool() const
        {
            return std::get<bool>(storage);
        }

        double AsNumber() const
        {
            return std::get<double>(storage);
        }

        const std::string& AsString() const
        {
            return std::get<std::string>(storage);
        }

        const std::vector<JsonValue>& AsArray() const
        {
            return std::get<std::shared_ptr<JsonArray>>(storage)->values;
        }

        const std::map<std::string, JsonValue, std::less<>>& AsObject() const
        {
            return std::get<std::shared_ptr<JsonObject>>(storage)->values;
        }

        const JsonValue* Find(std::string_view key) const
        {
            if (!IsObject())
            {
                return nullptr;
            }

            const auto& object = AsObject();
            const auto iterator = object.find(key);
            return (iterator != object.end()) ? &iterator->second : nullptr;
        }

        Storage storage;
    };

    class JsonParser
    {
    public:
        explicit JsonParser(std::string_view source) :
            m_source(source),
            m_position(0)
        {
        }

        JsonValue Parse()
        {
            JsonValue value = ParseValue();
            SkipWhitespace();
            Require(m_position == m_source.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unexpected trailing JSON content.");
            return value;
        }

    private:
        JsonValue ParseValue()
        {
            SkipWhitespace();
            Require(m_position < m_source.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unexpected end of JSON input.");

            const char character = m_source[m_position];
            switch (character)
            {
            case '{':
                return ParseObject();
            case '[':
                return ParseArray();
            case '"':
                return JsonValue(ParseString());
            case 't':
                ConsumeKeyword("true");
                return JsonValue(true);
            case 'f':
                ConsumeKeyword("false");
                return JsonValue(false);
            case 'n':
                ConsumeKeyword("null");
                return JsonValue(nullptr);
            default:
                if (character == '-' || std::isdigit(static_cast<unsigned char>(character)) != 0)
                {
                    return JsonValue(ParseNumber());
                }

                Throw(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON token.");
            }
        }

        JsonValue ParseObject()
        {
            Expect('{');

            auto object = std::make_shared<JsonObject>();

            SkipWhitespace();
            if (TryConsume('}'))
            {
                return JsonValue(object);
            }

            while (true)
            {
                SkipWhitespace();
                Require(Peek() == '"', HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected JSON object key.");
                std::string key = ParseString();

                SkipWhitespace();
                Expect(':');

                object->values.emplace(std::move(key), ParseValue());

                SkipWhitespace();
                if (TryConsume('}'))
                {
                    break;
                }

                Expect(',');
            }

            return JsonValue(object);
        }

        JsonValue ParseArray()
        {
            Expect('[');

            auto array = std::make_shared<JsonArray>();

            SkipWhitespace();
            if (TryConsume(']'))
            {
                return JsonValue(array);
            }

            while (true)
            {
                array->values.push_back(ParseValue());

                SkipWhitespace();
                if (TryConsume(']'))
                {
                    break;
                }

                Expect(',');
            }

            return JsonValue(array);
        }

        std::string ParseString()
        {
            Expect('"');

            std::string result;
            while (m_position < m_source.size())
            {
                const char character = m_source[m_position++];
                if (character == '"')
                {
                    return result;
                }

                if (character == '\\')
                {
                    Require(m_position < m_source.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON escape.");
                    const char escapeCharacter = m_source[m_position++];
                    switch (escapeCharacter)
                    {
                    case '"':
                    case '\\':
                    case '/':
                        result.push_back(escapeCharacter);
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u':
                        AppendCodePoint(ParseUnicodeEscape(), result);
                        break;
                    default:
                        Throw(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unsupported JSON escape sequence.");
                    }
                }
                else
                {
                    result.push_back(character);
                }
            }

            Throw(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unterminated JSON string.");
        }

        double ParseNumber()
        {
            const size_t start = m_position;

            if (Peek() == '-')
            {
                ++m_position;
            }

            if (Peek() == '0')
            {
                ++m_position;
            }
            else
            {
                ConsumeDigits();
            }

            if (Peek() == '.')
            {
                ++m_position;
                ConsumeDigits();
            }

            const char exponentCharacter = Peek();
            if (exponentCharacter == 'e' || exponentCharacter == 'E')
            {
                ++m_position;
                const char signCharacter = Peek();
                if (signCharacter == '+' || signCharacter == '-')
                {
                    ++m_position;
                }

                ConsumeDigits();
            }

            std::string valueString(m_source.substr(start, m_position - start));
            char* endPointer = nullptr;
            errno = 0;
            const double value = std::strtod(valueString.c_str(), &endPointer);
            Require(endPointer == valueString.c_str() + valueString.size() && errno == 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON number.");
            return value;
        }

        void SkipWhitespace()
        {
            while (m_position < m_source.size() &&
                std::isspace(static_cast<unsigned char>(m_source[m_position])) != 0)
            {
                ++m_position;
            }
        }

        void ConsumeKeyword(std::string_view keyword)
        {
            Require(m_source.substr(m_position, keyword.size()) == keyword, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON keyword.");
            m_position += keyword.size();
        }

        void ConsumeDigits()
        {
            const size_t start = m_position;
            while (m_position < m_source.size() &&
                std::isdigit(static_cast<unsigned char>(m_source[m_position])) != 0)
            {
                ++m_position;
            }

            Require(m_position > start, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected digit in JSON number.");
        }

        void Expect(char character)
        {
            Require(TryConsume(character), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unexpected JSON character.");
        }

        bool TryConsume(char character)
        {
            SkipWhitespace();
            if (m_position < m_source.size() && m_source[m_position] == character)
            {
                ++m_position;
                return true;
            }

            return false;
        }

        char Peek() const
        {
            return (m_position < m_source.size()) ? m_source[m_position] : '\0';
        }

        std::uint32_t ParseUnicodeEscape()
        {
            Require(m_position + 4 <= m_source.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON unicode escape.");

            std::uint32_t codePoint = 0;
            for (int i = 0; i < 4; ++i)
            {
                codePoint <<= 4;
                const char character = m_source[m_position++];
                if (character >= '0' && character <= '9')
                {
                    codePoint |= static_cast<std::uint32_t>(character - '0');
                }
                else if (character >= 'A' && character <= 'F')
                {
                    codePoint |= static_cast<std::uint32_t>(character - 'A' + 10);
                }
                else if (character >= 'a' && character <= 'f')
                {
                    codePoint |= static_cast<std::uint32_t>(character - 'a' + 10);
                }
                else
                {
                    Throw(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid JSON unicode escape.");
                }
            }

            return codePoint;
        }

        static void AppendCodePoint(std::uint32_t codePoint, std::string& output)
        {
            if (codePoint <= 0x7F)
            {
                output.push_back(static_cast<char>(codePoint));
                return;
            }

            if (codePoint <= 0x7FF)
            {
                output.push_back(static_cast<char>(0xC0u | ((codePoint >> 6) & 0x1Fu)));
                output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
                return;
            }

            if (codePoint <= 0xFFFF)
            {
                output.push_back(static_cast<char>(0xE0u | ((codePoint >> 12) & 0x0Fu)));
                output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
                output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
                return;
            }

            output.push_back(static_cast<char>(0xF0u | ((codePoint >> 18) & 0x07u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }

    private:
        std::string_view m_source;
        size_t m_position;
    };

    struct DocumentData
    {
        JsonValue root;
        std::vector<std::uint8_t> binaryChunk;
        fs::path baseDirectory;
    };

    struct BufferViewDef
    {
        int bufferIndex = -1;
        size_t byteOffset = 0;
        size_t byteLength = 0;
        size_t byteStride = 0;
    };

    struct AccessorDef
    {
        int bufferViewIndex = -1;
        size_t byteOffset = 0;
        int componentType = 0;
        size_t count = 0;
        std::string type;
        bool normalized = false;
    };

    struct TextureDef
    {
        int sourceImageIndex = -1;
    };

    struct MeshDef
    {
        std::vector<size_t> primitiveIndices;
    };

    struct NodeDef
    {
        XMFLOAT4X4 localMatrix = {};
        int meshIndex = -1;
        std::vector<int> children;
    };

    XMFLOAT4X4 StoreMatrix(const XMMATRIX& matrix)
    {
        XMFLOAT4X4 result = {};
        XMStoreFloat4x4(&result, matrix);
        return result;
    }

    XMFLOAT4X4 IdentityMatrix()
    {
        return StoreMatrix(XMMatrixIdentity());
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    std::vector<std::uint8_t> ReadBinaryFile(const fs::path& filePath)
    {
        std::ifstream input(filePath, std::ios::binary);
        Require(input.good(), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Failed to open file: " + filePath.string());

        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        Require(size >= 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Failed to read file size: " + filePath.string());
        input.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty())
        {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            Require(input.good(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Failed to read file: " + filePath.string());
        }

        return bytes;
    }

    std::uint32_t ReadUInt32LittleEndian(const std::uint8_t* data)
    {
        return
            static_cast<std::uint32_t>(data[0]) |
            (static_cast<std::uint32_t>(data[1]) << 8u) |
            (static_cast<std::uint32_t>(data[2]) << 16u) |
            (static_cast<std::uint32_t>(data[3]) << 24u);
    }

    std::string_view SkipUtf8Bom(std::string_view text)
    {
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            return text.substr(3);
        }

        return text;
    }

    DocumentData LoadDocument(const fs::path& filePath)
    {
        DocumentData document;
        document.baseDirectory = filePath.parent_path();

        const std::vector<std::uint8_t> fileBytes = ReadBinaryFile(filePath);
        const std::string extension = ToLowerAscii(filePath.extension().string());

        if (extension == ".glb")
        {
            Require(fileBytes.size() >= 20, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "GLB file is too small.");

            const std::uint32_t magic = ReadUInt32LittleEndian(fileBytes.data());
            const std::uint32_t version = ReadUInt32LittleEndian(fileBytes.data() + 4);
            const std::uint32_t length = ReadUInt32LittleEndian(fileBytes.data() + 8);

            Require(magic == 0x46546C67u, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid GLB magic.");
            Require(version == 2u, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Only glTF 2.0 / GLB 2 is supported.");
            Require(length <= fileBytes.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "GLB length exceeds file size.");

            size_t offset = 12;
            std::string jsonChunk;

            while (offset + 8 <= length)
            {
                const std::uint32_t chunkLength = ReadUInt32LittleEndian(fileBytes.data() + offset);
                const std::uint32_t chunkType = ReadUInt32LittleEndian(fileBytes.data() + offset + 4);
                offset += 8;

                Require(offset + chunkLength <= length, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Corrupted GLB chunk.");

                const std::uint8_t* chunkData = fileBytes.data() + offset;
                if (chunkType == 0x4E4F534Au)
                {
                    jsonChunk.assign(reinterpret_cast<const char*>(chunkData), reinterpret_cast<const char*>(chunkData + chunkLength));
                }
                else if (chunkType == 0x004E4942u && document.binaryChunk.empty())
                {
                    document.binaryChunk.assign(chunkData, chunkData + chunkLength);
                }

                offset += chunkLength;
            }

            Require(!jsonChunk.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "GLB file is missing a JSON chunk.");
            document.root = JsonParser(SkipUtf8Bom(jsonChunk)).Parse();
            return document;
        }

        const std::string jsonText(fileBytes.begin(), fileBytes.end());
        document.root = JsonParser(SkipUtf8Bom(jsonText)).Parse();
        return document;
    }

    const JsonValue& GetRequiredMember(const JsonValue& objectValue, std::string_view key, const char* context)
    {
        const JsonValue* value = objectValue.Find(key);
        Require(value != nullptr, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), std::string(context) + " is missing required key '" + std::string(key) + "'.");
        return *value;
    }

    const JsonValue* GetOptionalMember(const JsonValue& objectValue, std::string_view key)
    {
        return objectValue.Find(key);
    }

    int GetOptionalInt(const JsonValue& objectValue, std::string_view key, int defaultValue)
    {
        const JsonValue* value = objectValue.Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }

        Require(value->IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected numeric JSON property.");
        return static_cast<int>(std::llround(value->AsNumber()));
    }

    size_t GetOptionalSize(const JsonValue& objectValue, std::string_view key, size_t defaultValue)
    {
        const JsonValue* value = objectValue.Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }

        Require(value->IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected numeric JSON property.");
        return static_cast<size_t>(std::llround(value->AsNumber()));
    }

    float GetOptionalFloat(const JsonValue& objectValue, std::string_view key, float defaultValue)
    {
        const JsonValue* value = objectValue.Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }

        Require(value->IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected numeric JSON property.");
        return static_cast<float>(value->AsNumber());
    }

    bool GetOptionalBool(const JsonValue& objectValue, std::string_view key, bool defaultValue)
    {
        const JsonValue* value = objectValue.Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }

        Require(value->IsBool(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected boolean JSON property.");
        return value->AsBool();
    }

    std::string GetOptionalString(const JsonValue& objectValue, std::string_view key, const std::string& defaultValue)
    {
        const JsonValue* value = objectValue.Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }

        Require(value->IsString(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected string JSON property.");
        return value->AsString();
    }

    template <size_t Count>
    std::array<float, Count> ReadFloatArray(const JsonValue& value, const std::array<float, Count>& defaultValue)
    {
        if (value.IsNull())
        {
            return defaultValue;
        }

        Require(value.IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected JSON array.");
        const auto& array = value.AsArray();
        Require(array.size() == Count, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unexpected JSON array length.");

        std::array<float, Count> result = {};
        for (size_t index = 0; index < Count; ++index)
        {
            Require(array[index].IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expected numeric array element.");
            result[index] = static_cast<float>(array[index].AsNumber());
        }

        return result;
    }

    std::string PercentDecode(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());

        for (size_t index = 0; index < value.size(); ++index)
        {
            const char character = value[index];
            if (character == '%' && index + 2 < value.size())
            {
                auto hexToValue = [](char input) -> int
                    {
                        if (input >= '0' && input <= '9')
                        {
                            return input - '0';
                        }

                        if (input >= 'A' && input <= 'F')
                        {
                            return input - 'A' + 10;
                        }

                        if (input >= 'a' && input <= 'f')
                        {
                            return input - 'a' + 10;
                        }

                        return -1;
                    };

                const int high = hexToValue(value[index + 1]);
                const int low = hexToValue(value[index + 2]);
                if (high >= 0 && low >= 0)
                {
                    result.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                    continue;
                }
            }

            result.push_back(character);
        }

        return result;
    }

    int DecodeBase64Character(char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return character - 'A';
        }

        if (character >= 'a' && character <= 'z')
        {
            return character - 'a' + 26;
        }

        if (character >= '0' && character <= '9')
        {
            return character - '0' + 52;
        }

        if (character == '+')
        {
            return 62;
        }

        if (character == '/')
        {
            return 63;
        }

        return -1;
    }

    std::vector<std::uint8_t> DecodeBase64(std::string_view encoded)
    {
        std::vector<std::uint8_t> result;
        int accumulator = 0;
        int bitCount = 0;

        for (const char character : encoded)
        {
            if (std::isspace(static_cast<unsigned char>(character)) != 0)
            {
                continue;
            }

            if (character == '=')
            {
                break;
            }

            const int value = DecodeBase64Character(character);
            Require(value >= 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid base64 data URI.");

            accumulator = (accumulator << 6) | value;
            bitCount += 6;

            if (bitCount >= 8)
            {
                bitCount -= 8;
                result.push_back(static_cast<std::uint8_t>((accumulator >> bitCount) & 0xFF));
            }
        }

        return result;
    }

    std::vector<std::uint8_t> LoadBufferUri(const std::string& uri, const fs::path& baseDirectory)
    {
        constexpr std::string_view kDataUriPrefix = "data:";
        if (uri.rfind(kDataUriPrefix, 0) == 0)
        {
            const size_t commaPosition = uri.find(',');
            Require(commaPosition != std::string::npos, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Malformed data URI.");

            const std::string_view metadata(uri.data() + kDataUriPrefix.size(), commaPosition - kDataUriPrefix.size());
            const std::string_view payload(uri.data() + commaPosition + 1, uri.size() - commaPosition - 1);
            Require(metadata.find(";base64") != std::string_view::npos, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Only base64-encoded data URIs are supported.");
            return DecodeBase64(payload);
        }

        return ReadBinaryFile(baseDirectory / fs::path(PercentDecode(uri)));
    }

    std::vector<std::uint8_t> LoadBufferViewBytes(
        int bufferViewIndex,
        const std::vector<BufferViewDef>& bufferViews,
        const std::vector<std::vector<std::uint8_t>>& buffers)
    {
        Require(bufferViewIndex >= 0 && static_cast<size_t>(bufferViewIndex) < bufferViews.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view index is out of range.");
        const BufferViewDef& bufferView = bufferViews[static_cast<size_t>(bufferViewIndex)];
        Require(bufferView.bufferIndex >= 0 && static_cast<size_t>(bufferView.bufferIndex) < buffers.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view buffer index is out of range.");

        const std::vector<std::uint8_t>& buffer = buffers[static_cast<size_t>(bufferView.bufferIndex)];
        Require(bufferView.byteOffset + bufferView.byteLength <= buffer.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view exceeds buffer size.");

        return std::vector<std::uint8_t>(
            buffer.begin() + static_cast<std::ptrdiff_t>(bufferView.byteOffset),
            buffer.begin() + static_cast<std::ptrdiff_t>(bufferView.byteOffset + bufferView.byteLength));
    }

    Gltf::AlphaMode ParseAlphaMode(const std::string& value)
    {
        if (value.empty() || value == "OPAQUE")
        {
            return Gltf::AlphaMode::Opaque;
        }

        if (value == "MASK")
        {
            return Gltf::AlphaMode::Mask;
        }

        if (value == "BLEND")
        {
            return Gltf::AlphaMode::Blend;
        }

        Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported glTF alphaMode: " + value);
    }

    Gltf::TextureRef ParseTextureReference(
        const JsonValue* textureInfoValue,
        const std::vector<TextureDef>& textures)
    {
        Gltf::TextureRef textureRef = {};
        if (textureInfoValue == nullptr)
        {
            return textureRef;
        }

        Require(textureInfoValue->IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Texture info must be an object.");
        const int textureIndex = GetOptionalInt(*textureInfoValue, "index", -1);
        if (textureIndex < 0)
        {
            return textureRef;
        }

        Require(static_cast<size_t>(textureIndex) < textures.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Texture index is out of range.");
        textureRef.imageIndex = textures[static_cast<size_t>(textureIndex)].sourceImageIndex;
        textureRef.texCoord = GetOptionalInt(*textureInfoValue, "texCoord", 0);
        Require(textureRef.texCoord == 0, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Only TEXCOORD_0 is supported.");
        return textureRef;
    }

    size_t GetTypeComponentCount(const std::string& type)
    {
        if (type == "SCALAR")
        {
            return 1;
        }

        if (type == "VEC2")
        {
            return 2;
        }

        if (type == "VEC3")
        {
            return 3;
        }

        if (type == "VEC4")
        {
            return 4;
        }

        Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported accessor type: " + type);
    }

    size_t GetComponentSize(int componentType)
    {
        switch (componentType)
        {
        case 5120:
        case 5121:
            return 1;
        case 5122:
        case 5123:
            return 2;
        case 5125:
        case 5126:
            return 4;
        default:
            Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported accessor component type.");
        }
    }

    template <typename T>
    T ReadScalar(const std::uint8_t* data)
    {
        T value = {};
        std::memcpy(&value, data, sizeof(T));
        return value;
    }

    float ReadComponentAsFloat(const std::uint8_t* data, int componentType, bool normalized)
    {
        switch (componentType)
        {
        case 5120:
        {
            const std::int8_t value = ReadScalar<std::int8_t>(data);
            return normalized ? std::max(static_cast<float>(value) / 127.0f, -1.0f) : static_cast<float>(value);
        }
        case 5121:
        {
            const std::uint8_t value = ReadScalar<std::uint8_t>(data);
            return normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
        }
        case 5122:
        {
            const std::int16_t value = ReadScalar<std::int16_t>(data);
            return normalized ? std::max(static_cast<float>(value) / 32767.0f, -1.0f) : static_cast<float>(value);
        }
        case 5123:
        {
            const std::uint16_t value = ReadScalar<std::uint16_t>(data);
            return normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
        }
        case 5126:
            return ReadScalar<float>(data);
        default:
            Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported accessor component type.");
        }
    }

    std::vector<float> ReadAccessorFloats(
        int accessorIndex,
        size_t expectedComponents,
        const std::vector<AccessorDef>& accessors,
        const std::vector<BufferViewDef>& bufferViews,
        const std::vector<std::vector<std::uint8_t>>& buffers)
    {
        Require(accessorIndex >= 0 && static_cast<size_t>(accessorIndex) < accessors.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor index is out of range.");
        const AccessorDef& accessor = accessors[static_cast<size_t>(accessorIndex)];

        Require(accessor.bufferViewIndex >= 0 && static_cast<size_t>(accessor.bufferViewIndex) < bufferViews.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Sparse glTF accessors are not supported.");
        Require(GetTypeComponentCount(accessor.type) == expectedComponents, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unexpected accessor type.");

        const BufferViewDef& bufferView = bufferViews[static_cast<size_t>(accessor.bufferViewIndex)];
        Require(bufferView.bufferIndex >= 0 && static_cast<size_t>(bufferView.bufferIndex) < buffers.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view index is out of range.");

        const std::vector<std::uint8_t>& buffer = buffers[static_cast<size_t>(bufferView.bufferIndex)];
        const size_t componentSize = GetComponentSize(accessor.componentType);
        const size_t elementSize = expectedComponents * componentSize;
        const size_t stride = (bufferView.byteStride != 0) ? bufferView.byteStride : elementSize;
        const size_t startOffset = bufferView.byteOffset + accessor.byteOffset;

        Require(startOffset <= buffer.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor start offset exceeds buffer size.");
        Require(stride >= elementSize, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor stride is smaller than the element size.");
        Require(accessor.count == 0 || startOffset + stride * (accessor.count - 1u) + elementSize <= buffer.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor data exceeds buffer size.");

        std::vector<float> values(accessor.count * expectedComponents);
        const std::uint8_t* base = buffer.data() + startOffset;
        for (size_t elementIndex = 0; elementIndex < accessor.count; ++elementIndex)
        {
            const std::uint8_t* elementData = base + stride * elementIndex;
            for (size_t componentIndex = 0; componentIndex < expectedComponents; ++componentIndex)
            {
                values[elementIndex * expectedComponents + componentIndex] =
                    ReadComponentAsFloat(elementData + componentIndex * componentSize, accessor.componentType, accessor.normalized);
            }
        }

        return values;
    }

    std::vector<UINT> ReadAccessorIndices(
        int accessorIndex,
        const std::vector<AccessorDef>& accessors,
        const std::vector<BufferViewDef>& bufferViews,
        const std::vector<std::vector<std::uint8_t>>& buffers)
    {
        Require(accessorIndex >= 0 && static_cast<size_t>(accessorIndex) < accessors.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor index is out of range.");
        const AccessorDef& accessor = accessors[static_cast<size_t>(accessorIndex)];

        Require(accessor.bufferViewIndex >= 0 && static_cast<size_t>(accessor.bufferViewIndex) < bufferViews.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Sparse glTF accessors are not supported.");
        Require(GetTypeComponentCount(accessor.type) == 1, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Index accessor must be scalar.");

        const BufferViewDef& bufferView = bufferViews[static_cast<size_t>(accessor.bufferViewIndex)];
        Require(bufferView.bufferIndex >= 0 && static_cast<size_t>(bufferView.bufferIndex) < buffers.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view index is out of range.");

        const std::vector<std::uint8_t>& buffer = buffers[static_cast<size_t>(bufferView.bufferIndex)];
        const size_t componentSize = GetComponentSize(accessor.componentType);
        const size_t stride = (bufferView.byteStride != 0) ? bufferView.byteStride : componentSize;
        const size_t startOffset = bufferView.byteOffset + accessor.byteOffset;

        Require(startOffset <= buffer.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Index accessor start offset exceeds buffer size.");
        Require(stride >= componentSize, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Index accessor stride is smaller than the component size.");
        Require(accessor.count == 0 || startOffset + stride * (accessor.count - 1u) + componentSize <= buffer.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Index accessor exceeds buffer size.");

        std::vector<UINT> indices(accessor.count);
        const std::uint8_t* base = buffer.data() + startOffset;
        for (size_t index = 0; index < accessor.count; ++index)
        {
            const std::uint8_t* componentData = base + stride * index;
            switch (accessor.componentType)
            {
            case 5121:
                indices[index] = static_cast<UINT>(ReadScalar<std::uint8_t>(componentData));
                break;
            case 5123:
                indices[index] = static_cast<UINT>(ReadScalar<std::uint16_t>(componentData));
                break;
            case 5125:
                indices[index] = ReadScalar<std::uint32_t>(componentData);
                break;
            default:
                Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported index component type.");
            }
        }

        return indices;
    }

    std::vector<XMFLOAT3> ComputeNormals(const std::vector<XMFLOAT3>& positions, const std::vector<UINT>& indices)
    {
        std::vector<XMFLOAT3> normals(positions.size(), XMFLOAT3(0.0f, 0.0f, 0.0f));
        for (size_t index = 0; index + 2 < indices.size(); index += 3)
        {
            const UINT i0 = indices[index];
            const UINT i1 = indices[index + 1];
            const UINT i2 = indices[index + 2];
            Require(i0 < positions.size() && i1 < positions.size() && i2 < positions.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Primitive index is out of range.");

            const XMVECTOR p0 = XMLoadFloat3(&positions[i0]);
            const XMVECTOR p1 = XMLoadFloat3(&positions[i1]);
            const XMVECTOR p2 = XMLoadFloat3(&positions[i2]);
            const XMVECTOR faceNormal = XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0));

            XMVECTOR accumulated = XMLoadFloat3(&normals[i0]);
            accumulated = XMVectorAdd(accumulated, faceNormal);
            XMStoreFloat3(&normals[i0], accumulated);

            accumulated = XMLoadFloat3(&normals[i1]);
            accumulated = XMVectorAdd(accumulated, faceNormal);
            XMStoreFloat3(&normals[i1], accumulated);

            accumulated = XMLoadFloat3(&normals[i2]);
            accumulated = XMVectorAdd(accumulated, faceNormal);
            XMStoreFloat3(&normals[i2], accumulated);
        }

        for (XMFLOAT3& normal : normals)
        {
            const XMVECTOR normalized = XMVector3Normalize(XMLoadFloat3(&normal));
            XMStoreFloat3(&normal, normalized);
        }

        return normals;
    }

    Gltf::Primitive BuildPrimitive(
        const JsonValue& primitiveValue,
        const std::vector<AccessorDef>& accessors,
        const std::vector<BufferViewDef>& bufferViews,
        const std::vector<std::vector<std::uint8_t>>& buffers,
        size_t materialCount)
    {
        Require(primitiveValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Primitive definition must be a JSON object.");

        const JsonValue& attributesValue = GetRequiredMember(primitiveValue, "attributes", "glTF primitive");
        Require(attributesValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Primitive attributes must be a JSON object.");

        const int mode = GetOptionalInt(primitiveValue, "mode", 4);
        Require(mode == 4, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Only triangle glTF primitives are supported.");

        const int positionAccessorIndex = GetOptionalInt(attributesValue, "POSITION", -1);
        Require(positionAccessorIndex >= 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Primitive is missing POSITION data.");

        const int normalAccessorIndex = GetOptionalInt(attributesValue, "NORMAL", -1);
        const int texCoordAccessorIndex = GetOptionalInt(attributesValue, "TEXCOORD_0", -1);
        const int indicesAccessorIndex = GetOptionalInt(primitiveValue, "indices", -1);
        const int materialIndex = GetOptionalInt(primitiveValue, "material", 0);

        std::vector<float> positionValues = ReadAccessorFloats(positionAccessorIndex, 3, accessors, bufferViews, buffers);
        std::vector<float> normalValues;
        if (normalAccessorIndex >= 0)
        {
            normalValues = ReadAccessorFloats(normalAccessorIndex, 3, accessors, bufferViews, buffers);
            Require(normalValues.size() == positionValues.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "POSITION and NORMAL accessor sizes do not match.");
        }

        std::vector<float> texCoordValues;
        if (texCoordAccessorIndex >= 0)
        {
            texCoordValues = ReadAccessorFloats(texCoordAccessorIndex, 2, accessors, bufferViews, buffers);
            Require(texCoordValues.size() / 2u == positionValues.size() / 3u, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "POSITION and TEXCOORD_0 accessor sizes do not match.");
        }

        std::vector<UINT> indices;
        if (indicesAccessorIndex >= 0)
        {
            indices = ReadAccessorIndices(indicesAccessorIndex, accessors, bufferViews, buffers);
        }
        else
        {
            const size_t vertexCount = positionValues.size() / 3u;
            indices.resize(vertexCount);
            for (size_t index = 0; index < vertexCount; ++index)
            {
                indices[index] = static_cast<UINT>(index);
            }
        }

        Require(indices.size() % 3u == 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Triangle primitive index count must be divisible by three.");

        const size_t vertexCount = positionValues.size() / 3u;
        std::vector<XMFLOAT3> positions(vertexCount);
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texCoords(vertexCount, XMFLOAT2(0.0f, 0.0f));

        for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            positions[vertexIndex] =
                XMFLOAT3(
                    positionValues[vertexIndex * 3u + 0u],
                    positionValues[vertexIndex * 3u + 1u],
                    -positionValues[vertexIndex * 3u + 2u]);

            if (!normalValues.empty())
            {
                normals.push_back(
                    XMFLOAT3(
                        normalValues[vertexIndex * 3u + 0u],
                        normalValues[vertexIndex * 3u + 1u],
                        -normalValues[vertexIndex * 3u + 2u]));
            }

            if (!texCoordValues.empty())
            {
                texCoords[vertexIndex] =
                    XMFLOAT2(
                        texCoordValues[vertexIndex * 2u + 0u],
                        texCoordValues[vertexIndex * 2u + 1u]);
            }
        }

        for (size_t triangle = 0; triangle < indices.size(); triangle += 3)
        {
            std::swap(indices[triangle + 1u], indices[triangle + 2u]);
        }

        if (normals.empty())
        {
            normals = ComputeNormals(positions, indices);
        }
        else
        {
            for (XMFLOAT3& normal : normals)
            {
                const XMVECTOR normalized = XMVector3Normalize(XMLoadFloat3(&normal));
                XMStoreFloat3(&normal, normalized);
            }
        }

        Gltf::Primitive primitive;
        primitive.materialIndex =
            (materialIndex >= 0 && static_cast<size_t>(materialIndex) < materialCount)
            ? static_cast<size_t>(materialIndex)
            : 0u;

        primitive.vertices.resize(vertexCount);
        for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            primitive.vertices[vertexIndex] =
            {
                positions[vertexIndex],
                normals[vertexIndex],
                texCoords[vertexIndex],
            };
        }

        primitive.indices = std::move(indices);
        return primitive;
    }

    XMMATRIX ConvertRightHandedMatrixToLeftHanded(const XMMATRIX& matrix)
    {
        const XMMATRIX handedness = XMMatrixScaling(1.0f, 1.0f, -1.0f);
        return handedness * matrix * handedness;
    }

    XMMATRIX ParseNodeLocalMatrix(const JsonValue& nodeValue)
    {
        if (const JsonValue* matrixValue = GetOptionalMember(nodeValue, "matrix"))
        {
            const std::array<float, 16> values = ReadFloatArray<16>(*matrixValue, {});
            XMFLOAT4X4 rowMajorMatrix = {};
            rowMajorMatrix.m[0][0] = values[0];
            rowMajorMatrix.m[0][1] = values[4];
            rowMajorMatrix.m[0][2] = values[8];
            rowMajorMatrix.m[0][3] = values[12];
            rowMajorMatrix.m[1][0] = values[1];
            rowMajorMatrix.m[1][1] = values[5];
            rowMajorMatrix.m[1][2] = values[9];
            rowMajorMatrix.m[1][3] = values[13];
            rowMajorMatrix.m[2][0] = values[2];
            rowMajorMatrix.m[2][1] = values[6];
            rowMajorMatrix.m[2][2] = values[10];
            rowMajorMatrix.m[2][3] = values[14];
            rowMajorMatrix.m[3][0] = values[3];
            rowMajorMatrix.m[3][1] = values[7];
            rowMajorMatrix.m[3][2] = values[11];
            rowMajorMatrix.m[3][3] = values[15];
            return ConvertRightHandedMatrixToLeftHanded(XMLoadFloat4x4(&rowMajorMatrix));
        }

        const std::array<float, 3> translationValues =
            GetOptionalMember(nodeValue, "translation") != nullptr
            ? ReadFloatArray<3>(*GetOptionalMember(nodeValue, "translation"), { 0.0f, 0.0f, 0.0f })
            : std::array<float, 3>{ 0.0f, 0.0f, 0.0f };

        const std::array<float, 4> rotationValues =
            GetOptionalMember(nodeValue, "rotation") != nullptr
            ? ReadFloatArray<4>(*GetOptionalMember(nodeValue, "rotation"), { 0.0f, 0.0f, 0.0f, 1.0f })
            : std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };

        const std::array<float, 3> scaleValues =
            GetOptionalMember(nodeValue, "scale") != nullptr
            ? ReadFloatArray<3>(*GetOptionalMember(nodeValue, "scale"), { 1.0f, 1.0f, 1.0f })
            : std::array<float, 3>{ 1.0f, 1.0f, 1.0f };

        const XMMATRIX localMatrix =
            XMMatrixScaling(scaleValues[0], scaleValues[1], scaleValues[2]) *
            XMMatrixRotationQuaternion(XMVectorSet(rotationValues[0], rotationValues[1], rotationValues[2], rotationValues[3])) *
            XMMatrixTranslation(translationValues[0], translationValues[1], translationValues[2]);

        return ConvertRightHandedMatrixToLeftHanded(localMatrix);
    }

    void AppendNodePrimitivesRecursive(
        int nodeIndex,
        const std::vector<NodeDef>& nodes,
        const std::vector<MeshDef>& meshes,
        const XMMATRIX& parentWorld,
        std::vector<Gltf::NodePrimitive>& nodePrimitives)
    {
        Require(nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < nodes.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node index is out of range.");

        const NodeDef& node = nodes[static_cast<size_t>(nodeIndex)];
        const XMMATRIX localMatrix = XMLoadFloat4x4(&node.localMatrix);
        const XMMATRIX worldMatrix = localMatrix * parentWorld;

        if (node.meshIndex >= 0)
        {
            Require(static_cast<size_t>(node.meshIndex) < meshes.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node mesh index is out of range.");
            const MeshDef& mesh = meshes[static_cast<size_t>(node.meshIndex)];
            for (const size_t primitiveIndex : mesh.primitiveIndices)
            {
                Gltf::NodePrimitive nodePrimitive = {};
                nodePrimitive.primitiveIndex = primitiveIndex;
                nodePrimitive.world = StoreMatrix(worldMatrix);
                nodePrimitives.push_back(nodePrimitive);
            }
        }

        for (const int childIndex : node.children)
        {
            AppendNodePrimitivesRecursive(childIndex, nodes, meshes, worldMatrix, nodePrimitives);
        }
    }

    void ComputeSceneBounds(Gltf::Scene& scene)
    {
        bool hasAnyPoint = false;
        XMFLOAT3 boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
        XMFLOAT3 boundsMax(
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max());

        for (const Gltf::NodePrimitive& nodePrimitive : scene.nodePrimitives)
        {
            Require(nodePrimitive.primitiveIndex < scene.primitives.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node primitive index is out of range.");
            const Gltf::Primitive& primitive = scene.primitives[nodePrimitive.primitiveIndex];
            const XMMATRIX worldMatrix = XMLoadFloat4x4(&nodePrimitive.world);

            for (const Gltf::Vertex& vertex : primitive.vertices)
            {
                const XMVECTOR transformedPosition = XMVector3TransformCoord(XMLoadFloat3(&vertex.position), worldMatrix);
                XMFLOAT3 position = {};
                XMStoreFloat3(&position, transformedPosition);

                boundsMin.x = std::min(boundsMin.x, position.x);
                boundsMin.y = std::min(boundsMin.y, position.y);
                boundsMin.z = std::min(boundsMin.z, position.z);
                boundsMax.x = std::max(boundsMax.x, position.x);
                boundsMax.y = std::max(boundsMax.y, position.y);
                boundsMax.z = std::max(boundsMax.z, position.z);
                hasAnyPoint = true;
            }
        }

        if (!hasAnyPoint)
        {
            boundsMin = XMFLOAT3(-1.0f, -1.0f, -1.0f);
            boundsMax = XMFLOAT3(1.0f, 1.0f, 1.0f);
        }

        scene.boundsMin = boundsMin;
        scene.boundsMax = boundsMax;
    }

    std::string HResultToHex(HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(hr);
        return stream.str();
    }

}

HRESULT Gltf::LoadScene(const fs::path& filePath, Scene& scene)
{
    scene = {};
    g_lastErrorMessage.clear();

    try
    {
        const DocumentData document = LoadDocument(filePath);
        const JsonValue& root = document.root;
        Require(root.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "glTF root must be a JSON object.");

        const JsonValue& assetValue = GetRequiredMember(root, "asset", "glTF");
        Require(assetValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "glTF asset definition must be an object.");
        const std::string version = GetOptionalString(assetValue, "version", "");
        Require(!version.empty() && version.rfind("2.", 0) == 0, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Only glTF 2.0 assets are supported.");

        std::vector<std::vector<std::uint8_t>> buffers;
        if (const JsonValue* buffersValue = GetOptionalMember(root, "buffers"))
        {
            Require(buffersValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "buffers must be a JSON array.");
            const auto& bufferArray = buffersValue->AsArray();
            buffers.reserve(bufferArray.size());

            for (size_t bufferIndex = 0; bufferIndex < bufferArray.size(); ++bufferIndex)
            {
                const JsonValue& bufferValue = bufferArray[bufferIndex];
                Require(bufferValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer definition must be a JSON object.");

                const size_t declaredLength = GetOptionalSize(bufferValue, "byteLength", 0u);
                std::vector<std::uint8_t> bytes;
                if (const JsonValue* uriValue = GetOptionalMember(bufferValue, "uri"))
                {
                    Require(uriValue->IsString(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer URI must be a string.");
                    bytes = LoadBufferUri(uriValue->AsString(), document.baseDirectory);
                }
                else
                {
                    Require(bufferIndex == 0 && !document.binaryChunk.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer is missing a URI and no GLB binary chunk is available.");
                    bytes = document.binaryChunk;
                }

                Require(bytes.size() >= declaredLength, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer byteLength exceeds the loaded buffer size.");
                buffers.push_back(std::move(bytes));
            }
        }

        std::vector<BufferViewDef> bufferViews;
        if (const JsonValue* bufferViewsValue = GetOptionalMember(root, "bufferViews"))
        {
            Require(bufferViewsValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "bufferViews must be a JSON array.");
            const auto& bufferViewArray = bufferViewsValue->AsArray();
            bufferViews.reserve(bufferViewArray.size());

            for (const JsonValue& bufferViewValue : bufferViewArray)
            {
                Require(bufferViewValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Buffer view definition must be an object.");

                BufferViewDef bufferView = {};
                bufferView.bufferIndex = GetOptionalInt(bufferViewValue, "buffer", -1);
                bufferView.byteOffset = GetOptionalSize(bufferViewValue, "byteOffset", 0u);
                bufferView.byteLength = GetOptionalSize(bufferViewValue, "byteLength", 0u);
                bufferView.byteStride = GetOptionalSize(bufferViewValue, "byteStride", 0u);
                bufferViews.push_back(bufferView);
            }
        }

        std::vector<AccessorDef> accessors;
        if (const JsonValue* accessorsValue = GetOptionalMember(root, "accessors"))
        {
            Require(accessorsValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "accessors must be a JSON array.");
            const auto& accessorArray = accessorsValue->AsArray();
            accessors.reserve(accessorArray.size());

            for (const JsonValue& accessorValue : accessorArray)
            {
                Require(accessorValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor definition must be an object.");

                AccessorDef accessor = {};
                accessor.bufferViewIndex = GetOptionalInt(accessorValue, "bufferView", -1);
                accessor.byteOffset = GetOptionalSize(accessorValue, "byteOffset", 0u);
                accessor.componentType = GetOptionalInt(accessorValue, "componentType", 0);
                accessor.count = GetOptionalSize(accessorValue, "count", 0u);
                accessor.type = GetOptionalString(accessorValue, "type", "");
                accessor.normalized = GetOptionalBool(accessorValue, "normalized", false);

                Require(accessor.componentType != 0 && !accessor.type.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Accessor is missing required properties.");
                accessors.push_back(accessor);
            }
        }

        if (const JsonValue* imagesValue = GetOptionalMember(root, "images"))
        {
            Require(imagesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "images must be a JSON array.");
            scene.images.reserve(imagesValue->AsArray().size());

            for (size_t imageIndex = 0; imageIndex < imagesValue->AsArray().size(); ++imageIndex)
            {
                const JsonValue& imageValue = imagesValue->AsArray()[imageIndex];
                Require(imageValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Image definition must be an object.");

                Gltf::Image image = {};
                image.mimeType = GetOptionalString(imageValue, "mimeType", "");
                const std::string imageName = GetOptionalString(imageValue, "name", "Image" + std::to_string(imageIndex));
                image.debugName.assign(imageName.begin(), imageName.end());

                if (const JsonValue* uriValue = GetOptionalMember(imageValue, "uri"))
                {
                    Require(uriValue->IsString(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Image URI must be a string.");
                    image.encodedBytes = LoadBufferUri(uriValue->AsString(), document.baseDirectory);
                }
                else
                {
                    const int bufferViewIndex = GetOptionalInt(imageValue, "bufferView", -1);
                    Require(bufferViewIndex >= 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Image is missing both uri and bufferView.");
                    image.encodedBytes = LoadBufferViewBytes(bufferViewIndex, bufferViews, buffers);
                    Require(!image.mimeType.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "bufferView-backed images must define mimeType.");
                }

                Require(!image.encodedBytes.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Image data is empty.");
                scene.images.push_back(std::move(image));
            }
        }

        std::vector<TextureDef> textures;
        if (const JsonValue* texturesValue = GetOptionalMember(root, "textures"))
        {
            Require(texturesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "textures must be a JSON array.");
            textures.reserve(texturesValue->AsArray().size());

            for (const JsonValue& textureValue : texturesValue->AsArray())
            {
                Require(textureValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Texture definition must be an object.");

                TextureDef texture = {};
                texture.sourceImageIndex = GetOptionalInt(textureValue, "source", -1);
                Require(texture.sourceImageIndex >= 0, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Texture is missing source image.");
                Require(static_cast<size_t>(texture.sourceImageIndex) < scene.images.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Texture source image index is out of range.");
                textures.push_back(texture);
            }
        }

        if (const JsonValue* materialsValue = GetOptionalMember(root, "materials"))
        {
            Require(materialsValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "materials must be a JSON array.");
            for (const JsonValue& materialValue : materialsValue->AsArray())
            {
                Require(materialValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Material definition must be a JSON object.");

                if (const JsonValue* extensionsValue = GetOptionalMember(materialValue, "extensions"))
                {
                    if (const JsonValue* specGlossValue = extensionsValue->Find("KHR_materials_pbrSpecularGlossiness"))
                    {
                        UNREFERENCED_PARAMETER(specGlossValue);
                        Throw(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Specular-glossiness materials are not supported. Use metallic-roughness glTF models.");
                    }
                }

                Material material = {};
                material.doubleSided = GetOptionalBool(materialValue, "doubleSided", false);
                material.alphaMode = ParseAlphaMode(GetOptionalString(materialValue, "alphaMode", "OPAQUE"));
                material.alphaCutoff = GetOptionalFloat(materialValue, "alphaCutoff", 0.5f);

                if (const JsonValue* pbrValue = GetOptionalMember(materialValue, "pbrMetallicRoughness"))
                {
                    Require(pbrValue->IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "pbrMetallicRoughness must be an object.");

                    if (const JsonValue* baseColorFactorValue = GetOptionalMember(*pbrValue, "baseColorFactor"))
                    {
                        const std::array<float, 4> values = ReadFloatArray<4>(*baseColorFactorValue, { 1.0f, 1.0f, 1.0f, 1.0f });
                        material.baseColorFactor = XMFLOAT4(values[0], values[1], values[2], values[3]);
                    }

                    material.baseColorTexture = ParseTextureReference(GetOptionalMember(*pbrValue, "baseColorTexture"), textures);
                    material.metallicRoughnessTexture = ParseTextureReference(GetOptionalMember(*pbrValue, "metallicRoughnessTexture"), textures);
                    material.metallicFactor = GetOptionalFloat(*pbrValue, "metallicFactor", 1.0f);
                    material.roughnessFactor = GetOptionalFloat(*pbrValue, "roughnessFactor", 1.0f);
                }

                if (const JsonValue* emissiveFactorValue = GetOptionalMember(materialValue, "emissiveFactor"))
                {
                    const std::array<float, 3> values = ReadFloatArray<3>(*emissiveFactorValue, { 0.0f, 0.0f, 0.0f });
                    material.emissiveFactor = XMFLOAT3(values[0], values[1], values[2]);
                }

                material.emissiveTexture = ParseTextureReference(GetOptionalMember(materialValue, "emissiveTexture"), textures);
                material.occlusionTexture = ParseTextureReference(GetOptionalMember(materialValue, "occlusionTexture"), textures);
                if (const JsonValue* occlusionTextureValue = GetOptionalMember(materialValue, "occlusionTexture"))
                {
                    material.occlusionStrength = GetOptionalFloat(*occlusionTextureValue, "strength", 1.0f);
                }

                scene.materials.push_back(material);
            }
        }

        if (scene.materials.empty())
        {
            scene.materials.push_back(Material{});
        }

        std::vector<MeshDef> meshes;
        if (const JsonValue* meshesValue = GetOptionalMember(root, "meshes"))
        {
            Require(meshesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "meshes must be a JSON array.");

            for (const JsonValue& meshValue : meshesValue->AsArray())
            {
                Require(meshValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Mesh definition must be an object.");
                const JsonValue& primitivesValue = GetRequiredMember(meshValue, "primitives", "Mesh");
                Require(primitivesValue.IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Mesh primitives must be an array.");

                MeshDef mesh = {};
                for (const JsonValue& primitiveValue : primitivesValue.AsArray())
                {
                    mesh.primitiveIndices.push_back(scene.primitives.size());
                    scene.primitives.push_back(BuildPrimitive(primitiveValue, accessors, bufferViews, buffers, scene.materials.size()));
                }

                meshes.push_back(std::move(mesh));
            }
        }

        Require(!scene.primitives.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "The glTF scene does not contain any supported mesh primitives.");

        std::vector<NodeDef> nodes;
        if (const JsonValue* nodesValue = GetOptionalMember(root, "nodes"))
        {
            Require(nodesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "nodes must be a JSON array.");
            nodes.reserve(nodesValue->AsArray().size());

            for (const JsonValue& nodeValue : nodesValue->AsArray())
            {
                Require(nodeValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node definition must be an object.");

                NodeDef node = {};
                node.localMatrix = StoreMatrix(ParseNodeLocalMatrix(nodeValue));
                node.meshIndex = GetOptionalInt(nodeValue, "mesh", -1);

                if (const JsonValue* childrenValue = GetOptionalMember(nodeValue, "children"))
                {
                    Require(childrenValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node children must be an array.");
                    for (const JsonValue& childValue : childrenValue->AsArray())
                    {
                        Require(childValue.IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node child index must be numeric.");
                        node.children.push_back(static_cast<int>(std::llround(childValue.AsNumber())));
                    }
                }

                nodes.push_back(node);
            }
        }

        std::vector<int> rootNodes;
        if (const JsonValue* scenesValue = GetOptionalMember(root, "scenes"))
        {
            Require(scenesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "scenes must be a JSON array.");
            const auto& sceneArray = scenesValue->AsArray();
            Require(!sceneArray.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "glTF scenes array is empty.");

            int selectedScene = GetOptionalInt(root, "scene", 0);
            if (selectedScene < 0 || static_cast<size_t>(selectedScene) >= sceneArray.size())
            {
                selectedScene = 0;
            }

            const JsonValue& selectedSceneValue = sceneArray[static_cast<size_t>(selectedScene)];
            Require(selectedSceneValue.IsObject(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Scene definition must be an object.");

            if (const JsonValue* rootNodesValue = GetOptionalMember(selectedSceneValue, "nodes"))
            {
                Require(rootNodesValue->IsArray(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Scene nodes must be an array.");
                for (const JsonValue& nodeIndexValue : rootNodesValue->AsArray())
                {
                    Require(nodeIndexValue.IsNumber(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Scene node index must be numeric.");
                    rootNodes.push_back(static_cast<int>(std::llround(nodeIndexValue.AsNumber())));
                }
            }
        }

        if (rootNodes.empty() && !nodes.empty())
        {
            std::vector<bool> hasParent(nodes.size(), false);
            for (const NodeDef& node : nodes)
            {
                for (const int childIndex : node.children)
                {
                    Require(childIndex >= 0 && static_cast<size_t>(childIndex) < nodes.size(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Node child index is out of range.");
                    hasParent[static_cast<size_t>(childIndex)] = true;
                }
            }

            for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
            {
                if (!hasParent[nodeIndex])
                {
                    rootNodes.push_back(static_cast<int>(nodeIndex));
                }
            }
        }

        if (!rootNodes.empty())
        {
            for (const int rootNodeIndex : rootNodes)
            {
                AppendNodePrimitivesRecursive(rootNodeIndex, nodes, meshes, XMMatrixIdentity(), scene.nodePrimitives);
            }
        }
        else
        {
            for (size_t primitiveIndex = 0; primitiveIndex < scene.primitives.size(); ++primitiveIndex)
            {
                NodePrimitive nodePrimitive = {};
                nodePrimitive.primitiveIndex = primitiveIndex;
                nodePrimitive.world = IdentityMatrix();
                scene.nodePrimitives.push_back(nodePrimitive);
            }
        }

        Require(!scene.nodePrimitives.empty(), HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "The glTF scene did not produce any drawable nodes.");

        scene.sourceFileName = filePath.filename().wstring();
        ComputeSceneBounds(scene);
        g_lastErrorMessage.clear();
        return S_OK;
    }
    catch (const LoaderException& exception)
    {
        const std::string message =
            "glTF loader failed (" + HResultToHex(exception.hr) + "): " + exception.what() + "\n";
        g_lastErrorMessage = exception.what();
        OutputDebugStringA(message.c_str());
        scene = {};
        return exception.hr;
    }
    catch (const std::exception& exception)
    {
        const std::string message =
            "glTF loader failed (0x80004005): " + std::string(exception.what()) + "\n";
        g_lastErrorMessage = exception.what();
        OutputDebugStringA(message.c_str());
        scene = {};
        return E_FAIL;
    }
}

std::wstring Gltf::GetLastErrorMessage()
{
    return ToWide(g_lastErrorMessage);
}
