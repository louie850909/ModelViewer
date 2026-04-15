#include "pch.h"
#include "Scene.h"

int Scene::AddMesh(std::shared_ptr<Mesh> mesh) {
    // 構造と変換の両方が変化した
    m_structureRevision++;
    m_transformRevision++;
    return m_nextMeshId++;
}

void Scene::RemoveMeshById(int meshId) {
    // 修正：ロックやGPU待機はRenderer側で行うため、ここは削除するだけ
    auto it = std::find_if(m_meshes.begin(), m_meshes.end(),
        [meshId](const MeshInstance& inst) { return inst.meshId == meshId; });
    if (it != m_meshes.end())
    {
        m_meshes.erase(it);
        // 構造と変換の両方が変化した
        m_structureRevision++;
        m_transformRevision++;
    }
}

MeshInstance* Scene::FindInstance(int globalIndex, int& outLocalIndex) {
    int meshId = globalIndex / MESH_NODE_STRIDE;
    outLocalIndex = globalIndex % MESH_NODE_STRIDE;
    for (auto& inst : m_meshes) {
        if (inst.meshId == meshId &&
            outLocalIndex >= 0 &&
            outLocalIndex < (int)inst.mesh->nodes.size())
            return &inst;
    }
    return nullptr;
}

int Scene::GetTotalNodeCount() const {
    int total = 0;
    for (const auto& inst : m_meshes)
        total += (int)inst.mesh->nodes.size();
    return total;
}

bool Scene::GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    const auto& node = inst->mesh->nodes[localIdx];
    outName = node.name;
    outParentGlobal = (node.parentIndex >= 0)
        ? inst->meshId * MESH_NODE_STRIDE + node.parentIndex
        : -1;
    return true;
}

bool Scene::GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    const auto& node = inst->mesh->nodes[localIdx];
    if (outT) { outT[0] = node.t[0]; outT[1] = node.t[1]; outT[2] = node.t[2]; }
    if (outR) { outR[0] = node.r[0]; outR[1] = node.r[1]; outR[2] = node.r[2]; outR[3] = node.r[3]; }
    if (outS) { outS[0] = node.s[0]; outS[1] = node.s[1]; outS[2] = node.s[2]; }
    return true;
}

bool Scene::SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    auto& node = inst->mesh->nodes[localIdx];
    if (inT) { node.t[0] = inT[0]; node.t[1] = inT[1]; node.t[2] = inT[2]; }
    if (inR) { node.r[0] = inR[0]; node.r[1] = inR[1]; node.r[2] = inR[2]; node.r[3] = inR[3]; }
    if (inS) { node.s[0] = inS[0]; node.s[1] = inS[1]; node.s[2] = inS[2]; }
    // Transform のみ変化したため、SBT の再構築は不要！
    m_transformRevision++;
    return true;
}

int Scene::AddLight(int type) {
    LightNode l;
    l.id = m_nextLightId++;
    l.type = type;
    m_lights.push_back(l);
    return l.id;
}

void Scene::RemoveLight(int id) {
    m_lights.erase(std::remove_if(m_lights.begin(), m_lights.end(),
        [id](const LightNode& l) { return l.id == id; }), m_lights.end());
}

LightNode* Scene::GetLight(int id) {
    for (auto& l : m_lights) {
        if (l.id == id) return &l;
    }
    return nullptr;
}