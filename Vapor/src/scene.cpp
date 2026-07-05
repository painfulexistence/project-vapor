#include "scene.hpp"
#include "fluid_volume.hpp"

using namespace Vapor;

void Scene::addMesh(std::shared_ptr<Mesh> mesh, const glm::mat4& transform) {
    mesh->vertexOffset = vertices.size();
    mesh->indexOffset = indices.size();
    mesh->vertexCount = mesh->vertices.size();
    mesh->indexCount = mesh->indices.size();
    vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
    indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
    stagedMeshes.push_back(mesh);
    stagedMeshTransforms.push_back(transform);
    if (mesh->material) {// TODO: check if material & images are already in the scene
        materials.push_back(mesh->material);
        if (mesh->material->albedoMap) images.push_back(mesh->material->albedoMap);
        if (mesh->material->normalMap) images.push_back(mesh->material->normalMap);
        if (mesh->material->metallicMap) images.push_back(mesh->material->metallicMap);
        if (mesh->material->roughnessMap) images.push_back(mesh->material->roughnessMap);
        if (mesh->material->occlusionMap) images.push_back(mesh->material->occlusionMap);
        if (mesh->material->displacementMap) images.push_back(mesh->material->displacementMap);
    }
}

auto Scene::createFluidVolume(Physics3D* physics, const FluidVolumeSettings& settings) -> std::shared_ptr<FluidVolume> {
    auto fluidVolume = std::make_shared<FluidVolume>(physics, settings);
    fluidVolumes.push_back(fluidVolume);
    return fluidVolume;
}

void Scene::addFluidVolume(std::shared_ptr<FluidVolume> fluidVolume) {
    fluidVolumes.push_back(fluidVolume);
}
