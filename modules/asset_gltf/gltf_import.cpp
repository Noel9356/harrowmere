#include "gltf_import.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace engine
{

namespace
{

std::array<float, 3> vec3FromArray(
    const JsonValue *value, const std::array<float, 3> &fallback
)
{
    if (value == nullptr || !value->isArray() || value->array.size() < 3)
    {
        return fallback;
    }
    return {
        static_cast<float>(value->array[0].numberOr(0.0)),
        static_cast<float>(value->array[1].numberOr(0.0)),
        static_cast<float>(value->array[2].numberOr(0.0)),
    };
}

std::array<float, 4> vec4FromArray(
    const JsonValue *value, const std::array<float, 4> &fallback
)
{
    if (value == nullptr || !value->isArray() || value->array.size() < 4)
    {
        return fallback;
    }
    return {
        static_cast<float>(value->array[0].numberOr(0.0)),
        static_cast<float>(value->array[1].numberOr(0.0)),
        static_cast<float>(value->array[2].numberOr(0.0)),
        static_cast<float>(value->array[3].numberOr(0.0)),
    };
}

// A JSON number that will become a byte offset, an index or a count must be
// a non-negative integer a size_t can hold - the raw field is a double, and
// every direct static_cast below used to be undefined behavior for a
// negative or fractional value (a crafted file could turn "byteOffset": -1
// into a wild pointer instead of a clean parse error). Centralizing the
// validation keeps every downstream cast provably in range: the return is
// std::nullopt for anything that is not an exact, finite, representable
// non-negative integer.
std::optional<std::size_t> jsonSizeT(
    const JsonValue *value
)
{
    if (value == nullptr || !value->isNumber())
    {
        return std::nullopt;
    }
    const double number = value->number;
    // The 2^53 ceiling is where doubles stop representing every integer;
    // anything above cannot be an exact byte offset or count anyway. The
    // size_t ceiling keeps the cast exact on 32-bit builds (where 2^53 does
    // not fit) - a value beyond it is rejected, never truncated.
    if (!std::isfinite(number) || number < 0.0 || number != std::floor(number)
        || number > static_cast<double>(std::numeric_limits<std::size_t>::max()))
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(number);
}

// One node's resolved world transform plus its glTF index.
struct NodeRecord
{
    GltfMat4 worldTransform{};
    bool hasMesh          = false;
    std::size_t meshIndex = 0;
    std::string name;
    std::vector<std::size_t> children;
};

// Reads one node's LOCAL transform (matrix or TRS, exactly as the glTF 2.0
// spec prescribes: when matrix is present, TRS MUST be ignored).
GltfMat4 localTransformOf(
    const JsonValue &node
)
{
    const JsonValue *matrix = node.find("matrix");
    if (matrix != nullptr && matrix->isArray() && matrix->array.size() == 16)
    {
        // Column-major fill: one vec4 column per glTF matrix column.
        GltfMat4 result{};
        for (std::size_t column = 0; column < 4; ++column)
        {
            result[static_cast<glm::length_t>(column)] = glm::vec4{
                static_cast<float>(matrix->array[column * 4u + 0u].numberOr(0.0)),
                static_cast<float>(matrix->array[column * 4u + 1u].numberOr(0.0)),
                static_cast<float>(matrix->array[column * 4u + 2u].numberOr(0.0)),
                static_cast<float>(matrix->array[column * 4u + 3u].numberOr(0.0)),
            };
        }
        return result;
    }
    const std::array<float, 3> translation = vec3FromArray(node.find("translation"), {0.0f, 0.0f, 0.0f});
    // glTF rotation is (x, y, z, w) with w last.
    const std::array<float, 4> rotation = vec4FromArray(node.find("rotation"), {0.0f, 0.0f, 0.0f, 1.0f});
    const std::array<float, 3> scale    = vec3FromArray(node.find("scale"), {1.0f, 1.0f, 1.0f});
    return gltfComposeTrs(translation, rotation, scale);
}

// Builds the node table with composed world transforms: one depth-first walk,
// each world transform computed exactly once from its parent's. The depth cap
// bounds the recursion: the visited[] guard stops cycles, but a legal-JSON
// chain of 500k nested "children" entries would otherwise exhaust the stack
// and crash the import.
constexpr std::size_t maxNodeHierarchyDepth = 10000;

void collectNodes(
    const JsonValue &nodesArray,
    std::size_t nodeIndex,
    const GltfMat4 &parentTransform,
    std::vector<NodeRecord> &outNodes,
    std::vector<bool> &visited,
    std::size_t depth,
    std::string &errorReport
)
{
    if (depth > maxNodeHierarchyDepth)
    {
        errorReport = "node hierarchy is deeper than " + std::to_string(maxNodeHierarchyDepth)
                    + " levels; refusing the import (a legal-JSON chain this deep would exhaust the stack)";
        return;
    }
    if (nodeIndex >= nodesArray.array.size())
    {
        errorReport = "node index " + std::to_string(nodeIndex) + " is out of range";
        return;
    }
    if (visited[nodeIndex])
    {
        // A cycle is a malformed file (the spec forbids it); cutting the walk
        // here keeps the import finite without a crash.
        return;
    }
    visited[nodeIndex] = true;

    const JsonValue &node = nodesArray.array[nodeIndex];
    NodeRecord record;
    const JsonValue *nodeName = node.find("name");
    record.name               = nodeName != nullptr ? nodeName->stringOr("") : "";
    record.worldTransform     = gltfMultiplyMatrix(parentTransform, localTransformOf(node));

    const JsonValue *mesh = node.find("mesh");
    if (mesh != nullptr && mesh->isNumber())
    {
        // Validated like every index read from the file: a negative or
        // fractional number is a malformed file, not a wild index.
        const std::size_t meshIndex = jsonSizeT(mesh).value_or(std::numeric_limits<std::size_t>::max());
        if (meshIndex == std::numeric_limits<std::size_t>::max())
        {
            errorReport = "node '" + record.name + "' has a malformed mesh index";
            return;
        }
        record.hasMesh   = true;
        record.meshIndex = meshIndex;
    }

    const JsonValue *children = node.find("children");
    if (children != nullptr && children->isArray())
    {
        for (const JsonValue &child : children->array)
        {
            if (child.isNumber())
            {
                const std::size_t childIndex = jsonSizeT(&child).value_or(std::numeric_limits<std::size_t>::max());
                if (childIndex == std::numeric_limits<std::size_t>::max())
                {
                    errorReport = "node '" + record.name + "' has a malformed child index";
                    return;
                }
                record.children.push_back(childIndex);
            }
        }
    }

    // Move the record into its slot. The old form copied every field to
    // locals first "because recursion may reallocate" - but outNodes is
    // pre-sized by the caller and only ever assigned by index here, so it
    // never reallocates and the slot's own fields are safe to read across
    // the recursion (which writes other slots only).
    outNodes[nodeIndex] = std::move(record);

    for (std::size_t child : outNodes[nodeIndex].children)
    {
        collectNodes(nodesArray, child, outNodes[nodeIndex].worldTransform, outNodes, visited, depth + 1, errorReport);
        if (!errorReport.empty())
        {
            return;
        }
    }
}

// Material factors of one glTF material index (or the default material).
struct ImportedMaterial
{
    std::array<float, 3> albedo = {0.8f, 0.8f, 0.8f};
    float roughness             = 1.0f;
    float metallic              = 0.0f;
    float emissive              = 0.0f;
};

ImportedMaterial materialOf(
    const JsonValue *materialsArray, std::size_t materialIndex
)
{
    ImportedMaterial imported;
    if (materialsArray == nullptr || !materialsArray->isArray() || materialIndex >= materialsArray->array.size())
    {
        return imported;
    }
    const JsonValue &material = materialsArray->array[materialIndex];
    const JsonValue *pbr      = material.find("pbrMetallicRoughness");
    if (pbr != nullptr && pbr->isObject())
    {
        // baseColorFactor is RGBA; the alpha channel has no counterpart in
        // the engine's material model and is ignored.
        const std::array<float, 3> baseColor = vec3FromArray(pbr->find("baseColorFactor"), {1.0f, 1.0f, 1.0f});
        imported.albedo                      = baseColor;
        // find() returns null when the key is absent - the spec DEFAULTS
        // (metallic 1.0, roughness 1.0) apply, exactly like any other
        // optional factor here. The earlier unguarded dereference of
        // find(...)->numberOr crashed on documents that omit either key.
        const JsonValue *metallic  = pbr->find("metallicFactor");
        const JsonValue *roughness = pbr->find("roughnessFactor");
        imported.metallic          = metallic != nullptr ? static_cast<float>(metallic->numberOr(1.0)) : 1.0f;
        imported.roughness         = roughness != nullptr ? static_cast<float>(roughness->numberOr(1.0)) : 1.0f;
    }
    const std::array<float, 3> emissiveFactor = vec3FromArray(material.find("emissiveFactor"), {0.0f, 0.0f, 0.0f});
    // The engine's emissive is scalar radiance (albedo * strength); the glTF
    // factor is RGB. The strongest channel drives the strength - an honest
    // approximation for the emissive surfaces content authors model.
    imported.emissive = std::max(emissiveFactor[0], std::max(emissiveFactor[1], emissiveFactor[2]));
    return imported;
}

// The material factors of a whole document, memoized: real glTF content
// shares one material across many primitives (per-mesh material rows are
// the common authoring shape), and the per-primitive materialOf call
// re-parsed the same JSON factors - four-plus linear member scans per
// primitive - for values that never change during an import. Built once
// before the node walk: slot i holds materialOf(materials, i) (the same
// parse materialOf ran, so the values are identical), and a primitive with
// no material index reads the default-material slot.
std::vector<ImportedMaterial> memoizeMaterials(
    const JsonValue *materialsArray
)
{
    std::vector<ImportedMaterial> imported(1); // slot 0: the default material
    if (materialsArray != nullptr && materialsArray->isArray())
    {
        imported.reserve(imported.size() + materialsArray->array.size());
        for (std::size_t index = 0; index < materialsArray->array.size(); ++index)
        {
            imported.push_back(materialOf(materialsArray, index));
        }
    }
    return imported;
}

// ---- Full-mesh fidelity: buffer decoding and accessor reading --------------

// Base64 value table: -1 outside the alphabet, '=' handled by the caller.
// Built once from the same range rules the branchy per-character helper used
// to apply, so the accepted alphabet is byte-for-byte identical.
const std::array<std::int8_t, 256> &base64ValueTable()
{
    static const std::array<std::int8_t, 256> table = [] {
        std::array<std::int8_t, 256> built{};
        built.fill(-1);
        for (int index = 0; index < 26; ++index)
        {
            built[static_cast<std::size_t>('A' + index)] = static_cast<std::int8_t>(index);
        }
        for (int index = 0; index < 26; ++index)
        {
            built[static_cast<std::size_t>('a' + index)] = static_cast<std::int8_t>(index + 26);
        }
        for (int index = 0; index < 10; ++index)
        {
            built[static_cast<std::size_t>('0' + index)] = static_cast<std::int8_t>(index + 52);
        }
        built[static_cast<std::size_t>('+')] = 62;
        built[static_cast<std::size_t>('/')] = 63;
        return built;
    }();
    return table;
}

} // namespace

bool gltfDecodeBase64(
    std::string_view text, std::vector<std::uint8_t> &outBytes, std::string &errorReport
)
{
    outBytes.clear();
    outBytes.reserve(text.size() / 4 * 3);
    const std::array<std::int8_t, 256> &valueOf = base64ValueTable();
    std::uint32_t accumulator                   = 0;
    int bits                                    = 0;
    int padding                                 = 0;
    std::size_t position                        = 0;
    const std::size_t size                      = text.size();
    while (position < size)
    {
        // Fast lane: runs of four clean alphabet characters (no padding seen
        // yet, no whitespace inside the window) decode 4 -> 3 bytes in
        // straight-line code - one table load per character instead of a
        // five-branch range chain, one shift-or per triple instead of the
        // per-byte accumulator dance. Whitespace, '=' and anything outside
        // the alphabet fall through to the per-character path below, which
        // applies the historical acceptance order exactly (skip whitespace,
        // count padding, refuse data after padding, refuse non-alphabet);
        // the fast lane only ever runs where that path would decode the
        // same bytes without erroring.
        if (padding == 0 && position + 4 <= size)
        {
            const int v0 = valueOf[static_cast<std::uint8_t>(text[position])];
            const int v1 = valueOf[static_cast<std::uint8_t>(text[position + 1])];
            const int v2 = valueOf[static_cast<std::uint8_t>(text[position + 2])];
            const int v3 = valueOf[static_cast<std::uint8_t>(text[position + 3])];
            if ((v0 | v1 | v2 | v3) >= 0)
            {
                const std::uint32_t quadruple = (static_cast<std::uint32_t>(v0) << 18)
                                              | (static_cast<std::uint32_t>(v1) << 12)
                                              | (static_cast<std::uint32_t>(v2) << 6) | static_cast<std::uint32_t>(v3);
                outBytes.push_back(static_cast<std::uint8_t>((quadruple >> 16) & 0xFFu));
                outBytes.push_back(static_cast<std::uint8_t>((quadruple >> 8) & 0xFFu));
                outBytes.push_back(static_cast<std::uint8_t>(quadruple & 0xFFu));
                position += 4;
                continue;
            }
        }
        const char character = text[position];
        ++position;
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
        {
            continue; // RFC 4648 allows wrapping whitespace; be liberal here
        }
        if (character == '=')
        {
            ++padding;
            continue;
        }
        if (padding > 0)
        {
            errorReport = "base64 payload has data characters after padding";
            return false;
        }
        const int value = valueOf[static_cast<std::uint8_t>(character)];
        if (value < 0)
        {
            errorReport = "base64 payload contains a character outside the alphabet";
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            outBytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    if (padding > 2)
    {
        errorReport = "base64 payload has more than two padding characters";
        return false;
    }
    return true;
}

namespace
{

// One primitive's decoded vertex data: positions and normals are per-vertex
// floats, indices reference them (empty when the primitive is non-indexed -
// the reader then treats the vertices as a triangle list in order).
struct MeshPrimitiveData
{
    std::vector<float> positions;       // 3 floats per vertex
    std::vector<float> normals;         // 3 floats per vertex; may be empty
    std::vector<std::uint32_t> indices; // triangle list; may be empty
};

// Grows the local-space AABB of one primitive from its POSITION accessor's
// min/max (both are JSON fields - the bounding-proxy path never decodes
// buffer views).
bool accessorBounds(
    const JsonValue &accessor, std::array<float, 3> &outMin, std::array<float, 3> &outMax
)
{
    const JsonValue *min = accessor.find("min");
    const JsonValue *max = accessor.find("max");
    if (min == nullptr || max == nullptr || !min->isArray() || !max->isArray() || min->array.size() < 3
        || max->array.size() < 3)
    {
        return false;
    }
    outMin = {
        static_cast<float>(min->array[0].numberOr(0.0)),
        static_cast<float>(min->array[1].numberOr(0.0)),
        static_cast<float>(min->array[2].numberOr(0.0)),
    };
    outMax = {
        static_cast<float>(max->array[0].numberOr(0.0)),
        static_cast<float>(max->array[1].numberOr(0.0)),
        static_cast<float>(max->array[2].numberOr(0.0)),
    };
    return true;
}

// The document's top-level tables the accessor readers need, resolved once
// per import: accessorSpan and accessorComponentType used to re-find
// "accessors" and "bufferViews" through a linear member scan of the document
// root PER ACCESSOR PER ATTRIBUTE PER PRIMITIVE (~5 scans x ~50 string
// compares each), even though the pointers never move for the run of an
// import - the same field-level double-find the accessor fields below
// already fixed, one level up.
struct GltfDocumentTables
{
    const JsonValue *accessors   = nullptr;
    const JsonValue *bufferViews = nullptr;
};

GltfDocumentTables resolveDocumentTables(
    const JsonValue &document
)
{
    GltfDocumentTables tables;
    tables.accessors   = document.find("accessors");
    tables.bufferViews = document.find("bufferViews");
    return tables;
}

// Looks up one accessor's declared componentType (the raw JSON number), or
// nullptr when the accessor does not exist: the index decode uses it to pick
// exactly the declared format instead of trial-decoding the buffer as one
// type after another.
const JsonValue *accessorComponentType(
    const GltfDocumentTables &tables, std::size_t accessorIndex
)
{
    const JsonValue *accessorsArray = tables.accessors;
    if (accessorsArray == nullptr || !accessorsArray->isArray() || accessorIndex >= accessorsArray->array.size())
    {
        return nullptr;
    }
    return accessorsArray->array[accessorIndex].find("componentType");
}

// Reads one accessor's raw span: resolves bufferView + buffer, validates the
// component/type pair, the alignment and the byte bounds - with CHECKED
// arithmetic, so the sum cannot wrap past the buffer size the way the old
// modular `viewOffset + accessorOffset + requiredBytes` comparison could -
// and returns the pointer into the decoded buffer. Everything the caller
// does with the data stays local - the decoded buffers live in the caller's
// vector for the whole import.
const std::uint8_t *accessorSpan(
    const GltfDocumentTables &tables,
    const std::vector<std::vector<std::uint8_t>> &bufferData,
    std::size_t accessorIndex,
    std::uint32_t expectedComponentType,
    const char *expectedType,
    std::size_t &outElementCount,
    std::size_t &outStrideBytes,
    const std::string &sourceName,
    std::string &errorReport
)
{
    const JsonValue *accessorsArray = tables.accessors;
    if (accessorsArray == nullptr || !accessorsArray->isArray() || accessorIndex >= accessorsArray->array.size())
    {
        errorReport = sourceName + ": accessor index " + std::to_string(accessorIndex) + " is out of range";
        return nullptr;
    }
    const JsonValue &accessor = accessorsArray->array[accessorIndex];
    // Each field is read once: the double-find pattern (find for the null
    // check, find again for the value) doubled the scan cost of every
    // accessor field. Runs per accessor per attribute per primitive.
    const JsonValue *countValue = accessor.find("count");
    const std::size_t count     = jsonSizeT(countValue).value_or(0);
    // A usable count is also a SANE count: the caller multiplies it into
    // vertex/index vector resizes downstream, so the cap (100 million
    // elements - far beyond any real model) keeps a crafted "count": 1e18
    // from turning a validated span into a multi-terabyte resize attempt.
    if (count == 0 || count > 100000000u)
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex) + " has no usable count";
        return nullptr;
    }
    const JsonValue *componentTypeValue = accessor.find("componentType");
    const std::size_t componentType     = jsonSizeT(componentTypeValue).value_or(0);
    const JsonValue *typeValue          = accessor.find("type");
    const std::string type              = typeValue != nullptr ? typeValue->stringOr("") : "";
    if (componentType != static_cast<std::size_t>(expectedComponentType)
        || (expectedType != nullptr && type != expectedType))
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex) + " is " + type + "/"
                    + std::to_string(componentType) + ", expected " + expectedType + "/"
                    + std::to_string(expectedComponentType);
        return nullptr;
    }
    const std::size_t componentSize  = expectedComponentType == 5126u ? 4u
                                     : expectedComponentType == 5125u ? 4u
                                     : expectedComponentType == 5123u ? 2u
                                                                      : 1u;
    const std::size_t componentCount = std::string_view(expectedType) == "VEC3"   ? 3u
                                     : std::string_view(expectedType) == "SCALAR" ? 1u
                                                                                  : 4u;
    const std::size_t elementSize    = componentSize * componentCount;

    const JsonValue *bufferViewValue  = accessor.find("bufferView");
    const std::size_t bufferViewIndex = jsonSizeT(bufferViewValue).value_or(std::numeric_limits<std::size_t>::max());
    const JsonValue *bufferViewsArray = tables.bufferViews;
    if (bufferViewsArray == nullptr || !bufferViewsArray->isArray()
        || bufferViewIndex >= bufferViewsArray->array.size())
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex) + " names no usable bufferView";
        return nullptr;
    }
    const JsonValue &bufferView       = bufferViewsArray->array[bufferViewIndex];
    const JsonValue *bufferIndexValue = bufferView.find("buffer");
    const std::size_t bufferIndex     = jsonSizeT(bufferIndexValue).value_or(std::numeric_limits<std::size_t>::max());
    if (bufferIndex >= bufferData.size())
    {
        errorReport = sourceName + ": bufferView " + std::to_string(bufferViewIndex) + " names no usable buffer";
        return nullptr;
    }
    const std::vector<std::uint8_t> &sourceBuffer = bufferData[bufferIndex];
    const std::size_t viewOffset                  = jsonSizeT(bufferView.find("byteOffset")).value_or(0);
    const std::size_t accessorOffset              = jsonSizeT(accessor.find("byteOffset")).value_or(0);
    // A bufferView.byteStride larger than the element size means the accessor
    // is interleaved with other attributes (the glTF vertex layout); the
    // element's own bytes stay contiguous, only the step grows. The spec
    // requires both offsets and the stride to be multiples of the component
    // size - enforced here, because the callers read elements through typed
    // pointers (float*, uint32_t*) and an unaligned base would be undefined
    // behavior on several of this engine's target architectures.
    const JsonValue *strideValue = bufferView.find("byteStride");
    const std::size_t byteStride = jsonSizeT(strideValue).value_or(0);
    outStrideBytes               = byteStride > elementSize ? byteStride : elementSize;
    outElementCount              = count;
    if ((viewOffset + accessorOffset) % componentSize != 0
        || (outStrideBytes > elementSize && outStrideBytes % componentSize != 0))
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex)
                    + " is not aligned to its component size (" + std::to_string(componentSize) + " bytes)";
        return nullptr;
    }
    // Bounds arithmetic without any wrapping: every step is formulated as a
    // subtraction from the buffer size (which cannot underflow after the
    // preceding comparisons), so a crafted huge offset can no longer wrap
    // its size_t sum back inside the buffer the way the old modular check
    // allowed.
    const std::size_t bufferSize = sourceBuffer.size();
    if (viewOffset > bufferSize || accessorOffset > bufferSize - viewOffset)
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex) + " reads past the end of its buffer";
        return nullptr;
    }
    const std::size_t spanBase  = viewOffset + accessorOffset;
    const std::size_t available = bufferSize - spanBase;
    // The last element starts at (count - 1) * stride and runs elementSize
    // bytes; the stride is at least elementSize (>= 1), so the division
    // below is always defined.
    bool fits = available >= elementSize;
    if (fits && outElementCount > 1)
    {
        fits = (outElementCount - 1) <= (available - elementSize) / outStrideBytes;
    }
    if (!fits)
    {
        errorReport = sourceName + ": accessor " + std::to_string(accessorIndex) + " reads past the end of its buffer";
        return nullptr;
    }
    return sourceBuffer.data() + spanBase;
}

// Reads one primitive's positions/normals/indices into MeshPrimitiveData.
// Positions are required; normals are optional (computed below when absent);
// indices are optional (sequential when absent). Only TRIANGLES (mode 4) is
// accepted at mesh fidelity - a mode this renderer cannot draw would be a
// silent lie.
bool readPrimitiveMesh(
    const GltfDocumentTables &tables,
    const JsonValue &primitive,
    const std::vector<std::vector<std::uint8_t>> &bufferData,
    MeshPrimitiveData &out,
    const std::string &sourceName,
    std::string &errorReport
)
{
    // One find, kept: the null check and the read share the result (the
    // double-find here was the last remaining instance of the pattern the
    // accessor fields below already fixed).
    const JsonValue *modeValue = primitive.find("mode");
    const double mode          = modeValue != nullptr ? modeValue->numberOr(4.0) : 4.0;
    if (mode != 4.0)
    {
        errorReport = sourceName + ": primitive mode " + std::to_string(static_cast<long long>(mode))
                    + ": only triangles (mode 4) are imported at mesh fidelity"
                    + " (bounds-only import accepts every primitive)";
        return false;
    }
    const JsonValue *attributes = primitive.find("attributes");
    if (attributes == nullptr || !attributes->isObject())
    {
        errorReport = sourceName + ": primitive has no attributes";
        return false;
    }
    const JsonValue *positionAccessor = attributes->find("POSITION");
    const std::size_t positionIndex   = jsonSizeT(positionAccessor).value_or(std::numeric_limits<std::size_t>::max());
    if (positionIndex == std::numeric_limits<std::size_t>::max())
    {
        errorReport = sourceName + ": primitive has no POSITION attribute";
        return false;
    }
    std::size_t positionCount         = 0;
    std::size_t positionStride        = 0;
    const std::uint8_t *positionBytes = accessorSpan(
        tables, bufferData, positionIndex, 5126u, "VEC3", positionCount, positionStride, sourceName, errorReport
    );
    if (positionBytes == nullptr)
    {
        return false;
    }
    out.positions.resize(positionCount * 3u);
    if (positionStride == 3u * sizeof(float))
    {
        // Tightly packed VEC3 floats - the layout virtually every exporter
        // writes (glTF 2.0 spec: an undefined bufferView.byteStride means
        // exactly this). The decode is then a pure byte copy: the per-vertex
        // gather below reads the same bytes one float at a time.
        std::memcpy(out.positions.data(), positionBytes, positionCount * 3u * sizeof(float));
    } else
    {
        for (std::size_t vertex = 0; vertex < positionCount; ++vertex)
        {
            const float *source             = reinterpret_cast<const float *>(positionBytes + vertex * positionStride);
            out.positions[vertex * 3u + 0u] = source[0];
            out.positions[vertex * 3u + 1u] = source[1];
            out.positions[vertex * 3u + 2u] = source[2];
        }
    }

    const JsonValue *normalAccessor = attributes->find("NORMAL");
    if (normalAccessor != nullptr && normalAccessor->isNumber())
    {
        std::size_t normalCount         = 0;
        std::size_t normalStride        = 0;
        const std::uint8_t *normalBytes = accessorSpan(
            tables,
            bufferData,
            jsonSizeT(normalAccessor).value_or(std::numeric_limits<std::size_t>::max()),
            5126u,
            "VEC3",
            normalCount,
            normalStride,
            sourceName,
            errorReport
        );
        if (normalBytes == nullptr)
        {
            return false;
        }
        if (normalCount != positionCount)
        {
            errorReport = sourceName + ": NORMAL accessor covers " + std::to_string(normalCount)
                        + " vertices but POSITION covers " + std::to_string(positionCount);
            return false;
        }
        out.normals.resize(normalCount * 3u);
        if (normalStride == 3u * sizeof(float))
        {
            // Tight packing: same byte-copy fast path as POSITION above.
            std::memcpy(out.normals.data(), normalBytes, normalCount * 3u * sizeof(float));
        } else
        {
            for (std::size_t vertex = 0; vertex < normalCount; ++vertex)
            {
                const float *source           = reinterpret_cast<const float *>(normalBytes + vertex * normalStride);
                out.normals[vertex * 3u + 0u] = source[0];
                out.normals[vertex * 3u + 1u] = source[1];
                out.normals[vertex * 3u + 2u] = source[2];
            }
        }
    }

    const JsonValue *indexAccessor = primitive.find("indices");
    if (indexAccessor != nullptr && indexAccessor->isNumber())
    {
        // The accessor's DECLARED componentType picks the decode format - the
        // old trial loop tried 5125/5123/5121 in order and cleared every
        // failure, which misreported a genuine out-of-bounds span as "no
        // supported component type" and would even decode a truncated 4-byte
        // accessor as bytes. One declaration, one format, errors propagate.
        struct IndexFormat
        {
            std::size_t componentType;
            std::size_t size;
        };
        const std::size_t declaredComponentType = jsonSizeT(
            accessorComponentType(tables, jsonSizeT(indexAccessor).value_or(std::numeric_limits<std::size_t>::max()))
        )
                                                      .value_or(0);
        const IndexFormat formats[]     = {{5125u, 4u}, {5123u, 2u}, {5121u, 1u}};
        const IndexFormat *chosenFormat = static_cast<const IndexFormat *>(nullptr);
        for (const IndexFormat &format : formats)
        {
            if (format.componentType == declaredComponentType)
            {
                chosenFormat = &format;
                break;
            }
        }
        if (chosenFormat == nullptr)
        {
            errorReport = sourceName + ": indices accessor declares component type "
                        + std::to_string(declaredComponentType)
                        + "; only unsigned byte (5121), unsigned short (5123) and unsigned int (5125) are supported";
            return false;
        }
        std::size_t indexCount         = 0;
        std::size_t indexStride        = 0;
        const std::uint8_t *indexBytes = accessorSpan(
            tables,
            bufferData,
            jsonSizeT(indexAccessor).value_or(std::numeric_limits<std::size_t>::max()),
            static_cast<std::uint32_t>(chosenFormat->componentType),
            "SCALAR",
            indexCount,
            indexStride,
            sourceName,
            errorReport
        );
        if (indexBytes == nullptr)
        {
            return false;
        }
        out.indices.resize(indexCount);
        // The per-index format branch is hoisted out of the loop: three
        // specialized walks, each still carrying the range check that
        // guards the import (stride == component size is the packed
        // case every exporter ships).
        if (chosenFormat->size == 4u)
        {
            for (std::size_t index = 0; index < indexCount; ++index)
            {
                const std::uint32_t value = *reinterpret_cast<const std::uint32_t *>(indexBytes + index * indexStride);
                if (value >= positionCount)
                {
                    errorReport = sourceName + ": index " + std::to_string(value) + " is out of range ("
                                + std::to_string(positionCount) + " vertices)";
                    return false;
                }
                out.indices[index] = value;
            }
        } else if (chosenFormat->size == 2u)
        {
            for (std::size_t index = 0; index < indexCount; ++index)
            {
                const std::uint32_t value = *reinterpret_cast<const std::uint16_t *>(indexBytes + index * indexStride);
                if (value >= positionCount)
                {
                    errorReport = sourceName + ": index " + std::to_string(value) + " is out of range ("
                                + std::to_string(positionCount) + " vertices)";
                    return false;
                }
                out.indices[index] = value;
            }
        } else
        {
            for (std::size_t index = 0; index < indexCount; ++index)
            {
                const std::uint32_t value = *(indexBytes + index * indexStride);
                if (value >= positionCount)
                {
                    errorReport = sourceName + ": index " + std::to_string(value) + " is out of range ("
                                + std::to_string(positionCount) + " vertices)";
                    return false;
                }
                out.indices[index] = value;
            }
        }
        return true;
    }
    // Non-indexed: the vertices are the triangle list in order.
    out.indices.resize(positionCount);
    for (std::size_t index = 0; index < positionCount; ++index)
    {
        out.indices[index] = static_cast<std::uint32_t>(index);
    }
    return true;
}

// Area-weighted smooth normals for a triangle soup without NORMAL data: one
// accumulation pass, then normalize. Degenerate triangles contribute nothing;
// a vertex no triangle touches keeps the up fallback.
void computeSmoothNormals(
    const std::vector<float> &positions, const std::vector<std::uint32_t> &indices, std::vector<float> &outNormals
)
{
    outNormals.assign(positions.size(), 0.0f);
    const std::size_t vertexCount = positions.size() / 3u;
    for (std::size_t triangle = 0; triangle + 2u < indices.size(); triangle += 3u)
    {
        const std::uint32_t i0 = indices[triangle + 0u];
        const std::uint32_t i1 = indices[triangle + 1u];
        const std::uint32_t i2 = indices[triangle + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            continue;
        }
        const float a[3]
            = {positions[i1 * 3u] - positions[i0 * 3u],
                positions[i1 * 3u + 1u] - positions[i0 * 3u + 1u],
                positions[i1 * 3u + 2u] - positions[i0 * 3u + 2u]};
        const float b[3]
            = {positions[i2 * 3u] - positions[i0 * 3u],
                positions[i2 * 3u + 1u] - positions[i0 * 3u + 1u],
                positions[i2 * 3u + 2u] - positions[i0 * 3u + 2u]};
        const glm::vec3 faceNormal = glm::cross(glm::make_vec3(a), glm::make_vec3(b));
        for (const std::uint32_t corner : {i0, i1, i2})
        {
            outNormals[corner * 3u + 0u] += faceNormal.x;
            outNormals[corner * 3u + 1u] += faceNormal.y;
            outNormals[corner * 3u + 2u] += faceNormal.z;
        }
    }
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        const glm::vec3 normal = glm::make_vec3(&outNormals[vertex * 3u]);
        const float length     = glm::length(normal);
        if (length > 1e-12f)
        {
            const glm::vec3 unit         = normal / length;
            outNormals[vertex * 3u + 0u] = unit.x;
            outNormals[vertex * 3u + 1u] = unit.y;
            outNormals[vertex * 3u + 2u] = unit.z;
        } else
        {
            outNormals[vertex * 3u + 0u] = 0.0f;
            outNormals[vertex * 3u + 1u] = 1.0f;
            outNormals[vertex * 3u + 2u] = 0.0f;
        }
    }
}

} // namespace

GltfMat4 gltfMultiplyMatrix(
    const GltfMat4 &a, const GltfMat4 &b
)
{
    // Column-major, result = a * b: GLM's operator* is exactly this product
    // over the same storage layout (the hand-rolled loop it replaces computed
    // result[column][row] = sum_k a[k][row] * b[column][k]).
    return a * b;
}

GltfMat4 gltfComposeTrs(
    const std::array<float, 3> &translation,
    const std::array<float, 4> &rotationQuaternion,
    const std::array<float, 3> &scale
)
{
    // translate * rotate * scale, the glTF TRS order. The quaternion arrives
    // in glTF's (x, y, z, w) layout while GLM's constructor takes (w, x, y,
    // z); glm::mat4_cast is the standard unit-quaternion-to-rotation matrix,
    // the same closed form the hand-rolled expansion computed.
    const glm::quat rotation{
        rotationQuaternion[3], rotationQuaternion[0], rotationQuaternion[1], rotationQuaternion[2]
    };
    return glm::translate(glm::mat4{1.0f}, glm::make_vec3(translation.data())) * glm::mat4_cast(rotation)
         * glm::scale(glm::mat4{1.0f}, glm::make_vec3(scale.data()));
}

std::array<float, 3> gltfTransformPoint(
    const GltfMat4 &matrix, const std::array<float, 3> &point
)
{
    // Homogeneous point transform (the w row is assumed 0 0 0 1, which the
    // compose above and glTF node matrices both guarantee).
    const glm::vec3 transformed = glm::vec3{matrix * glm::vec4{glm::make_vec3(point.data()), 1.0f}};
    return {transformed.x, transformed.y, transformed.z};
}

std::uint32_t gltfImportIntoScene(
    const JsonValue &document,
    SceneCapability &scene,
    const std::string &sourceName,
    std::uint32_t fidelity,
    std::string &errorReport,
    const std::vector<std::uint8_t> *glbBinaryBuffer
)
{
    // The three top-level arrays the importer consumes.
    const JsonValue *nodesArray     = document.find("nodes");
    const JsonValue *meshesArray    = document.find("meshes");
    const JsonValue *materialsArray = document.find("materials");
    if (nodesArray == nullptr || !nodesArray->isArray())
    {
        errorReport = sourceName + ": the document has no nodes array - nothing to import";
        return 0;
    }
    if (meshesArray == nullptr || !meshesArray->isArray())
    {
        errorReport = sourceName + ": the document has no meshes array - nothing to import";
        return 0;
    }

    // The root nodes: the default scene, or scene 0, or every parentless node
    // (the spec's fallback for files without a scenes array). The "scene"
    // lookup is read once: the old form called document.find twice (the value
    // and its isNumber probe), each a linear scan over the document's
    // members.
    const JsonValue *sceneIndexValue = document.find("scene");
    std::vector<std::size_t> rootIndices;
    const JsonValue *scenesArray = document.find("scenes");
    if (scenesArray != nullptr && scenesArray->isArray() && !scenesArray->array.empty())
    {
        const double sceneNumber
            = sceneIndexValue != nullptr && sceneIndexValue->isNumber() ? sceneIndexValue->number : 0.0;
        const std::size_t sceneIndex
            = sceneNumber >= 0.0 && sceneNumber < static_cast<double>(scenesArray->array.size())
                ? static_cast<std::size_t>(sceneNumber)
                : 0;
        const JsonValue *scene = &scenesArray->array[sceneIndex];
        const JsonValue *roots = scene->find("nodes");
        if (roots != nullptr && roots->isArray())
        {
            for (const JsonValue &root : roots->array)
            {
                // Validated, not cast: a negative or fractional root index
                // is malformed input, and a naive cast of a negative double
                // is formally undefined behavior before it is ever checked.
                const std::size_t rootIndex = jsonSizeT(&root).value_or(std::numeric_limits<std::size_t>::max());
                if (rootIndex < nodesArray->array.size())
                {
                    rootIndices.push_back(rootIndex);
                }
            }
        }
    }
    if (rootIndices.empty())
    {
        // Fallback: every node no other node lists as a child.
        std::vector<bool> isChild(nodesArray->array.size(), false);
        for (const JsonValue &node : nodesArray->array)
        {
            const JsonValue *children = node.find("children");
            if (children == nullptr || !children->isArray())
            {
                continue;
            }
            for (const JsonValue &child : children->array)
            {
                if (child.isNumber() && child.number >= 0.0 && child.number < static_cast<double>(isChild.size()))
                {
                    isChild[static_cast<std::size_t>(child.number)] = true;
                }
            }
        }
        for (std::size_t index = 0; index < isChild.size(); ++index)
        {
            if (!isChild[index])
            {
                rootIndices.push_back(index);
            }
        }
    }
    if (rootIndices.empty())
    {
        errorReport = sourceName + ": the document defines no root nodes";
        return 0;
    }

    // One world-transform walk over the whole hierarchy.
    std::vector<NodeRecord> nodes(nodesArray->array.size());
    std::vector<bool> visited(nodesArray->array.size(), false);
    const GltfMat4 identity{1.0f};
    for (std::size_t root : rootIndices)
    {
        collectNodes(*nodesArray, root, identity, nodes, visited, 0, errorReport);
        if (!errorReport.empty())
        {
            errorReport = sourceName + ": " + errorReport;
            return 0;
        }
    }

    // Spawn one object per mesh primitive, at the requested fidelity.
    std::uint32_t spawned = 0;

    // Resolved once (see GltfDocumentTables): the per-accessor re-finds of
    // "accessors"/"bufferViews" this replaced scanned the document root per
    // attribute per primitive.
    const GltfDocumentTables documentTables = resolveDocumentTables(document);
    // Material factors parsed once per document (see memoizeMaterials): the
    // per-primitive materialOf call re-parsed the same JSON factors for
    // every primitive that shared a material.
    const std::vector<ImportedMaterial> importedMaterials = memoizeMaterials(materialsArray);

    // Full-mesh fidelity needs the embedded buffers: decoded up front, once
    // per buffer (a document may reference several). External URIs are
    // refused: the .gltf JSON form stays self-contained (the module README
    // documents the mapping).
    const JsonValue *buffersArray = document.find("buffers");
    std::vector<std::vector<std::uint8_t>> bufferData;
    if (fidelity == asset_fidelity::full_mesh)
    {
        if (buffersArray == nullptr || !buffersArray->isArray() || buffersArray->array.empty())
        {
            errorReport = sourceName
                        + ": the document has no buffers array; full-mesh import needs embedded buffer data"
                        + " (use bounding-proxy fidelity, or export with embedded buffers)";
            return 0;
        }
        bufferData.resize(buffersArray->array.size());
        for (std::size_t bufferIndex = 0; bufferIndex < buffersArray->array.size(); ++bufferIndex)
        {
            const JsonValue &buffer = buffersArray->array[bufferIndex];
            // A view into the parsed value, not a copy: the embedded-buffer
            // URI of a full-mesh import is megabyte-scale base64, and the
            // old stringOr-then-substr chain copied it twice before decoding.
            const JsonValue *uriValue  = buffer.find("uri");
            const std::string_view uri = uriValue != nullptr ? uriValue->stringViewOr("") : std::string_view{};
            constexpr std::string_view dataPrefix   = "data:";
            constexpr std::string_view base64Marker = ";base64,";
            const std::size_t markerPosition        = uri.find(base64Marker);
            // A GLB container's buffer 0 with no URI IS the container's BIN
            // chunk (glTF 2.0 spec: "GLB-stored buffer", byteLength may sit
            // up to 3 bytes below the chunk length because of the 4-byte
            // chunk padding). The chunk arrives through glbBinaryBuffer.
            const bool glbStoredBuffer = uri.empty() && bufferIndex == 0 && glbBinaryBuffer != nullptr;
            if (uri.rfind(dataPrefix, 0) != 0 || markerPosition == std::string_view::npos)
            {
                if (glbStoredBuffer)
                {
                    // Honor the declared byteLength (padded bytes are not
                    // buffer data); fall back to the full chunk when the
                    // document omits it (the spec makes byteLength optional
                    // only for the GLB-stored buffer).
                    std::size_t byteLength    = glbBinaryBuffer->size();
                    const JsonValue *declared = buffer.find("byteLength");
                    if (declared != nullptr && declared->isNumber() && declared->number >= 0.0
                        && static_cast<double>(glbBinaryBuffer->size()) >= declared->number)
                    {
                        // The declared length is checked against the chunk's
                        // real size above, so the cast cannot exceed it; the
                        // integral check keeps a fractional value honest.
                        if (declared->number == std::floor(declared->number))
                        {
                            byteLength = static_cast<std::size_t>(declared->number);
                        }
                    }
                    bufferData[bufferIndex].assign(
                        glbBinaryBuffer->begin(), glbBinaryBuffer->begin() + static_cast<std::ptrdiff_t>(byteLength)
                    );
                    continue;
                }
                errorReport = sourceName + ": buffer " + std::to_string(bufferIndex)
                            + " references external data; full-mesh import reads embedded data only ("
                            + "data:application/octet-stream;base64,... in .gltf, the BIN chunk in .glb)";
                return 0;
            }
            std::string decodeError;
            if (!gltfDecodeBase64(
                    uri.substr(markerPosition + base64Marker.size()), bufferData[bufferIndex], decodeError
                ))
            {
                errorReport = sourceName + ": buffer " + std::to_string(bufferIndex) + ": " + decodeError;
                return 0;
            }
        }
    }

    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
    {
        const NodeRecord &node = nodes[nodeIndex];
        if (!node.hasMesh || node.meshIndex >= meshesArray->array.size())
        {
            continue;
        }
        const JsonValue &mesh       = meshesArray->array[node.meshIndex];
        const JsonValue *primitives = mesh.find("primitives");
        if (primitives == nullptr || !primitives->isArray())
        {
            continue;
        }
        std::size_t primitiveIndex = 0;
        for (const JsonValue &primitive : primitives->array)
        {
            // The memoized table (see memoizeMaterials): slot 0 is the
            // default material, slot i + 1 the document's material i; an
            // out-of-range index degrades to the default exactly like the
            // per-call materialOf's own bounds check did.
            const JsonValue *materialIndex = primitive.find("material");
            const std::size_t materialOrdinal
                = jsonSizeT(materialIndex).value_or(std::numeric_limits<std::size_t>::max());
            const ImportedMaterial &material = materialOrdinal < importedMaterials.size() - 1
                                                 ? importedMaterials[materialOrdinal + 1]
                                                 : importedMaterials[0];

            // ---- Full-mesh fidelity: register the triangles, instance them
            if (fidelity == asset_fidelity::full_mesh)
            {
                MeshPrimitiveData meshData;
                if (readPrimitiveMesh(documentTables, primitive, bufferData, meshData, sourceName, errorReport))
                {
                    // Bake the node transform into the vertices EXCEPT the
                    // translation: the engine's rotation model is Y-only, so
                    // a baked transform (any rotation, any scale) always
                    // lands exactly. The object's position carries the node
                    // origin plus the mesh centroid; the mesh data is
                    // CENTERED on the object's origin (the registry's proxy
                    // box and UV parameterization assume a centered mesh).
                    const std::array<float, 3> nodeOrigin = gltfTransformPoint(node.worldTransform, {0.0f, 0.0f, 0.0f});
                    const std::size_t vertexCount         = meshData.positions.size() / 3u;
                    std::vector<float> baked(meshData.positions.size());
                    std::array<float, 3> minimum{
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max()
                    };
                    std::array<float, 3> maximum{
                        -std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max()
                    };
                    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        const std::array<float, 3> world = gltfTransformPoint(
                            node.worldTransform,
                            {meshData.positions[vertex * 3u + 0u],
                                meshData.positions[vertex * 3u + 1u],
                                meshData.positions[vertex * 3u + 2u]}
                        );
                        for (std::size_t axis = 0; axis < 3; ++axis)
                        {
                            baked[vertex * 3u + axis] = world[axis] - nodeOrigin[axis];
                            minimum[axis]             = std::min(minimum[axis], baked[vertex * 3u + axis]);
                            maximum[axis]             = std::max(maximum[axis], baked[vertex * 3u + axis]);
                        }
                    }
                    const std::array<float, 3> centroid{
                        (minimum[0] + maximum[0]) * 0.5f,
                        (minimum[1] + maximum[1]) * 0.5f,
                        (minimum[2] + maximum[2]) * 0.5f
                    };
                    // Normals: rotate/scale with the vertices. The glTF data
                    // carries object-space normals; the baked transform's
                    // linear part applies through the inverse-transpose - for
                    // the transforms content ships (rigid, uniform scale)
                    // that is the same 3x3 renormalized, and the renderer
                    // normalizes per-vertex anyway. Recompute when the source
                    // had none. meshData's buffers are moved rather than
                    // copied: this primitive's read is their only consumer.
                    const bool hadNormals      = !meshData.normals.empty();
                    std::vector<float> normals = std::move(meshData.normals);
                    if (!hadNormals)
                    {
                        // The smooth-normals pass reads CENTERED positions
                        // (its cross products see the same operands the
                        // historical order fed it), so this path centers
                        // `baked` in place exactly like the standalone pass
                        // that used to run unconditionally.
                        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                        {
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                baked[vertex * 3u + axis] -= centroid[axis];
                            }
                        }
                        computeSmoothNormals(baked, meshData.indices, normals);
                    } else
                    {
                        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                        {
                            const glm::vec3 source{
                                normals[vertex * 3u + 0u], normals[vertex * 3u + 1u], normals[vertex * 3u + 2u]
                            };
                            // The world transform's upper 3x3 applied to the
                            // normal (w = 0 skips the translation), then
                            // normalized - exact for rigid + uniform scale.
                            // The hand-rolled loop this replaces read the
                            // matrix column-wise and therefore applied the
                            // TRANSPOSE: positions were transformed by M while
                            // authored normals were transformed by M-transpose,
                            // which only agrees with M when the rotation is
                            // symmetric (the identity most notably). GLM's
                            // mat4 * vec4 writes the row-dot form directly.
                            const glm::vec3 transformed
                                = glm::normalize(glm::vec3{node.worldTransform * glm::vec4{source, 0.0f}});
                            normals[vertex * 3u + 0u] = transformed.x;
                            normals[vertex * 3u + 1u] = transformed.y;
                            normals[vertex * 3u + 2u] = transformed.z;
                        }
                    }

                    // Interleave positions and normals into the registry's
                    // 6-float vertex layout. On the has-normals path the
                    // centering folds into this write (`baked - centroid` is
                    // the same single subtraction the standalone pass ran,
                    // one full array walk per primitive fewer). On the
                    // recomputed-normals path `baked` is already centered.
                    SceneMeshDescriptor meshDescriptor;
                    meshDescriptor.vertices.resize(vertexCount * 6u);
                    if (!hadNormals)
                    {
                        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                        {
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                meshDescriptor.vertices[vertex * 6u + axis]      = baked[vertex * 3u + axis];
                                meshDescriptor.vertices[vertex * 6u + 3u + axis] = normals[vertex * 3u + axis];
                            }
                        }
                    } else
                    {
                        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                        {
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                meshDescriptor.vertices[vertex * 6u + axis]
                                    = baked[vertex * 3u + axis] - centroid[axis];
                                meshDescriptor.vertices[vertex * 6u + 3u + axis] = normals[vertex * 3u + axis];
                            }
                        }
                    }
                    meshDescriptor.indices     = std::move(meshData.indices);
                    const std::string meshName = (node.name.empty() ? "mesh" : node.name) + "#n"
                                               + std::to_string(nodeIndex) + "p" + std::to_string(primitiveIndex);
                    const SceneCapability::MeshHandle meshHandle = scene.addMesh(meshName, meshDescriptor);
                    if (meshHandle != SceneCapability::invalidMesh)
                    {
                        SceneObjectDescriptor object;
                        object.type       = scene_object_type::mesh;
                        object.meshHandle = meshHandle;
                        object.position
                            = {nodeOrigin[0] + centroid[0], nodeOrigin[1] + centroid[1], nodeOrigin[2] + centroid[2]};
                        object.size
                            = {std::max((maximum[0] - minimum[0]) * 0.5f, 0.001f),
                                std::max((maximum[1] - minimum[1]) * 0.5f, 0.001f),
                                std::max((maximum[2] - minimum[2]) * 0.5f, 0.001f)};
                        object.albedo           = material.albedo;
                        object.roughness        = material.roughness;
                        object.metallic         = material.metallic;
                        object.emissiveStrength = material.emissive;
                        if (scene.addObject(object) == SceneCapability::invalidObject)
                        {
                            return spawned;
                        }
                        ++spawned;
                        ++primitiveIndex;
                        continue;
                    }
                    // invalidMesh: the mesh registry is full or refused the
                    // data (its refusal is logged precisely there). Fall
                    // through to the bounding-box proxy so the rest of the
                    // level still arrives - a documented partial import.
                } else
                {
                    // Structural read failure at mesh fidelity: precise
                    // failure of the whole import (the caller reports it).
                    errorReport = sourceName + ": " + errorReport;
                    return 0;
                }
            }

            // ---- Bounding-proxy fidelity (and the mesh registry's fallback):
            // one box from the POSITION accessor's min/max.
            const JsonValue *attributes = primitive.find("attributes");
            if (attributes == nullptr || !attributes->isObject())
            {
                ++primitiveIndex;
                continue;
            }
            const JsonValue *positionAccessor = attributes->find("POSITION");
            if (positionAccessor == nullptr || !positionAccessor->isNumber())
            {
                ++primitiveIndex;
                continue; // a primitive without positions draws nothing anywhere
            }
            // The bounds-only path reads the accessor's min/max, never its
            // bytes: the index still gets the validated lookup so a negative
            // or fractional POSITION reference skips the primitive like any
            // other malformed field instead of casting through UB first.
            const std::size_t boundsAccessorIndex
                = jsonSizeT(positionAccessor).value_or(std::numeric_limits<std::size_t>::max());
            const JsonValue *accessorsArray = documentTables.accessors;
            if (accessorsArray == nullptr || !accessorsArray->isArray()
                || boundsAccessorIndex >= accessorsArray->array.size())
            {
                ++primitiveIndex;
                continue;
            }
            std::array<float, 3> localMin{};
            std::array<float, 3> localMax{};
            if (!accessorBounds(accessorsArray->array[boundsAccessorIndex], localMin, localMax))
            {
                ++primitiveIndex;
                continue; // no min/max: the accessor was written without bounds
            }

            // All 8 corners through the world transform, then the world AABB
            // (a rotated box's bounds are the corners' bounds).
            glm::vec3 worldMin{std::numeric_limits<float>::max()};
            glm::vec3 worldMax{-std::numeric_limits<float>::max()};
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 local{
                    corner & 1 ? localMax[0] : localMin[0],
                    corner & 2 ? localMax[1] : localMin[1],
                    corner & 4 ? localMax[2] : localMin[2]
                };
                const glm::vec3 world = glm::vec3{node.worldTransform * glm::vec4{local, 1.0f}};
                worldMin              = glm::min(worldMin, world);
                worldMax              = glm::max(worldMax, world);
            }

            SceneObjectDescriptor object;
            object.type = scene_object_type::box;
            object.size
                = {std::max((worldMax[0] - worldMin[0]) * 0.5f, 0.001f),
                    std::max((worldMax[1] - worldMin[1]) * 0.5f, 0.001f),
                    std::max((worldMax[2] - worldMin[2]) * 0.5f, 0.001f)};
            object.position
                = {(worldMin[0] + worldMax[0]) * 0.5f,
                    (worldMin[1] + worldMax[1]) * 0.5f,
                    (worldMin[2] + worldMax[2]) * 0.5f};
            object.albedo           = material.albedo;
            object.roughness        = material.roughness;
            object.metallic         = material.metallic;
            object.emissiveStrength = material.emissive;

            if (scene.addObject(object) == SceneCapability::invalidObject)
            {
                // Registry full: report what arrived. The caller decides
                // whether that is acceptable for this asset.
                return spawned;
            }
            ++spawned;
            ++primitiveIndex;
        }
    }
    return spawned;
}

} // namespace engine
