#pragma once

#include <string>

#include "Asset/MeshData.h"

// 极简 OBJ 解析器。
//
// 支持：
//   - 指令 v / vt / vn / f
//   - 三角形与四边形面（四边形按扇形三角化）
//   - 顶点引用写法：v/vt/vn、v//vn、v/vt、v
// 不支持（刻意不做）：
//   - mtllib / usemtl（材质在渲染层由 Material 结构描述）
//   - 负索引、法线自动生成
//
// 为什么刻意保持极小：文件解析不是本项目重点（M6 重点是渲染路径）。
// 将来若要换成 glTF / cgltf，只要让新 loader 同样产出 MeshData，上层代码无需改动 ——
// 这就是把「解析」和「渲染」分开的价值。
bool LoadObjFromFile(const std::string& path, MeshData& outMesh, std::string& outError);
