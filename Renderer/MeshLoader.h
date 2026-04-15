#pragma once
#include "Mesh.h"
#include <memory>

class MeshLoader {
public:
    // 拡張子に基づいてロード方法を自動選択
    static std::shared_ptr<Mesh> Load(const std::string& path);

private:
    static std::shared_ptr<Mesh> LoadViaAssimp(const std::string& path);
    static std::shared_ptr<Mesh> LoadGltf(const std::string& path);
};