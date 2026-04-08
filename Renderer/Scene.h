#pragma once
#include "pch.h"
#include "Mesh.h"
#include <vector>
#include <memory>
#include <string>

struct SceneConstants {
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 unjitteredMvp;
    DirectX::XMFLOAT4X4 prevUnjitteredMvp;
    DirectX::XMFLOAT4X4 model;
};

struct LightNode {
    int id;
    int type; // 0=Directional, 1=Point, 2=Spot
    float intensity = 1.0f;
    float coneAngle = 30.0f;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float position[3] = { 0.0f, 5.0f, 0.0f };
    float direction[3] = { 0.0f, -1.0f, 0.0f };
};

struct MeshInstance {
    int meshId = -1;
    std::shared_ptr<Mesh> mesh;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    std::vector<ComPtr<ID3D12Resource>> textures;
    ComPtr<ID3D12Resource> vbUpload;
    ComPtr<ID3D12Resource> ibUpload;
};

constexpr int MESH_NODE_STRIDE = 10000;

class Scene {
public:
    int AddMesh(std::shared_ptr<Mesh> mesh); // 定義は .cpp へ移動
    void RemoveMeshById(int meshId);
    void AddMeshInstance(MeshInstance inst) { m_meshes.push_back(std::move(inst)); }

    void SetCameraTransform(float px, float py, float pz, float pitch, float yaw) {
        m_cameraPos = { px, py, pz }; m_pitch = pitch; m_yaw = yaw;
    }

    // ここを修正：const参照を返すようにする
    const DirectX::XMFLOAT3& GetCameraPos() const { return m_cameraPos; }
    float GetPitch() const { return m_pitch; }
    float GetYaw() const { return m_yaw; }

    int GetTotalNodeCount() const;
    bool GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal);
    bool GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS);
    bool SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS);

    int AddLight(int type);
    void RemoveLight(int id);
    LightNode* GetLight(int id);
    const std::vector<LightNode>& GetLights() const { return m_lights; }

    std::shared_ptr<Mesh> GetMesh() const { return m_meshes.empty() ? nullptr : m_meshes[0].mesh; }
    const std::vector<MeshInstance>& GetMeshes() const { return m_meshes; }

private:
    MeshInstance* FindInstance(int globalIndex, int& outLocalIndex);

    int m_nextMeshId = 0;
    std::vector<MeshInstance> m_meshes;
    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, 0.0f, -3.0f };
    float m_pitch = 0.0f;
    float m_yaw = 0.0f;

    int m_nextLightId = 0;
    std::vector<LightNode> m_lights;
};