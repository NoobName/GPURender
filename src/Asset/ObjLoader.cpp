#include "Asset/ObjLoader.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

// 按 '/' 切分 "v/vt/vn" 形式的顶点引用。
// 保留空片段，因为 "1//3" 和 "1/2" 的语义不同（前者没有 UV）。
std::vector<std::string> SplitBySlash(const std::string& token)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char c : token)
    {
        if (c == '/')
        {
            parts.push_back(current);
            current.clear();
        }
        else
        {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

// 把片段转成 OBJ 的 1-based 索引；空片段返回 0，表示「该分量不存在」。
int ParseIndex(const std::string& text)
{
    if (text.empty())
    {
        return 0;
    }
    return std::atoi(text.c_str());
}

} // namespace

bool LoadObjFromFile(const std::string& path, MeshData& outMesh, std::string& outError)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        outError = "OBJ: cannot open file: " + path;
        return false;
    }

    // OBJ 把位置 / UV / 法线存在三个互相独立的数组里，面再去引用它们的下标。
    // 而 GPU 需要「每个顶点的所有属性打包在一起」的数组，所以解析时必须重建顶点。
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT2> texcoords;
    std::vector<DirectX::XMFLOAT3> normals;

    // (position, uv, normal) 三元组 -> 已生成顶点下标。
    // OBJ 中相邻面会重复引用同一组属性，这里把它们合并成同一个顶点，
    // 既能减小顶点缓冲，也能让 GPU 的顶点缓存（post-transform cache）更有效。
    std::unordered_map<std::uint64_t, std::uint32_t> vertexCache;

    outMesh.vertices.clear();
    outMesh.indices.clear();

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::istringstream stream(line);
        std::string keyword;
        stream >> keyword;

        if (keyword == "v")
        {
            DirectX::XMFLOAT3 p = {};
            stream >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (keyword == "vt")
        {
            DirectX::XMFLOAT2 t = {};
            stream >> t.x >> t.y;
            texcoords.push_back(t);
        }
        else if (keyword == "vn")
        {
            DirectX::XMFLOAT3 n = {};
            stream >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (keyword == "f")
        {
            // 先把这一行所有的顶点引用解析出来（可能是三角形也可能是四边形），
            // 再统一做扇形三角化。
            std::vector<std::uint32_t> faceIndices;
            std::string token;
            while (stream >> token)
            {
                const std::vector<std::string> parts = SplitBySlash(token);
                const int positionIndex = parts.size() > 0 ? ParseIndex(parts[0]) : 0;
                const int uvIndex       = parts.size() > 1 ? ParseIndex(parts[1]) : 0;
                const int normalIndex   = parts.size() > 2 ? ParseIndex(parts[2]) : 0;

                if (positionIndex <= 0 || positionIndex > static_cast<int>(positions.size()))
                {
                    outError = "OBJ: line " + std::to_string(lineNumber) +
                               ": invalid position index in face";
                    return false;
                }
                if (uvIndex < 0 || normalIndex < 0)
                {
                    outError = "OBJ: line " + std::to_string(lineNumber) +
                               ": negative (relative) indices are not supported";
                    return false;
                }

                const std::uint64_t key =
                    (static_cast<std::uint64_t>(positionIndex) << 42) |
                    (static_cast<std::uint64_t>(uvIndex & 0x1FFFFF) << 21) |
                    static_cast<std::uint64_t>(normalIndex & 0x1FFFFF);

                const auto cached = vertexCache.find(key);
                if (cached != vertexCache.end())
                {
                    faceIndices.push_back(cached->second);
                    continue;
                }

                MeshVertex vertex = {};
                vertex.position = positions[positionIndex - 1];
                if (uvIndex > 0 && uvIndex <= static_cast<int>(texcoords.size()))
                {
                    vertex.uv = texcoords[uvIndex - 1];
                }
                if (normalIndex > 0 && normalIndex <= static_cast<int>(normals.size()))
                {
                    vertex.normal = normals[normalIndex - 1];
                }

                const std::uint32_t newIndex = static_cast<std::uint32_t>(outMesh.vertices.size());
                outMesh.vertices.push_back(vertex);
                vertexCache.emplace(key, newIndex);
                faceIndices.push_back(newIndex);
            }

            // 扇形三角化：n 边形 -> (n - 2) 个三角形。
            for (std::size_t i = 1; i + 1 < faceIndices.size(); ++i)
            {
                outMesh.indices.push_back(faceIndices[0]);
                outMesh.indices.push_back(faceIndices[i]);
                outMesh.indices.push_back(faceIndices[i + 1]);
            }
        }
        // 其它指令（o / g / s / usemtl / mtllib ...）与本项目无关，直接忽略。
    }

    if (outMesh.vertices.empty() || outMesh.indices.empty())
    {
        outError = "OBJ: no geometry found in " + path;
        return false;
    }
    return true;
}
