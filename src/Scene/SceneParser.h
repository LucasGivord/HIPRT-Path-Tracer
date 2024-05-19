/*
 * Copyright 2024 Tom Clabault. GNU GPL3 license.
 * GNU GPL3 license copy: https://www.gnu.org/licenses/gpl-3.0.txt
 */

#ifndef SCENE_PARSER_H
#define SCENE_PARSER_H

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"

#include "HostDeviceCommon/Material.h"
#include "Scene/Camera.h"
#include "Renderer/Sphere.h"
#include "Renderer/Triangle.h"

#include <thread>
#include <vector>

/**
 * Structure that holds the indices of the textures of a material during scene parsing
 */
struct ParsedMaterialTextureIndices
{
    int base_color_texture_index = -1;
    int emission_texture_index = -1;

    int roughness_texture_index = -1;
    int metallic_texture_index = -1;
    int roughness_metallic_texture_index = -1;

    int specular_texture_index = -1;
    int clearcoat_texture_index = -1;
    int sheen_texture_index = -1;
    int specular_transmission_texture_index = -1;

    int normal_map_texture_index = -1;
};

struct Scene
{
    std::vector<RendererMaterial> materials;
    // Material textures. Needs to be index by a material index. 
    std::vector<ImageRGBA> textures;
    // The widths and heights of the material textures
    // Necessary since Orochi doesn't support normalized texture coordinates
    // for texture object creation yet. This mean that we have to use texel coordinates
    // in [0, width - 1] and [0, height - 1] in the shader which means that we need the widths
    // and heights to convert UV coordinates [0, 1] to the right range
    std::vector<int2> textures_dims;
    // This vector of is sRGB indicates whether the texture is sRGB 
    // (and will need to be converted to linear in the shader or not)
    std::vector<unsigned char> textures_is_srgb;

    std::vector<int> triangle_indices;
    std::vector<float3> vertices_positions;
    std::vector<unsigned char> has_vertex_normals;
    std::vector<float3> vertex_normals;
    std::vector<float2> texcoords;
    std::vector<int> emissive_triangle_indices;
    std::vector<int> material_indices;

    bool has_camera = false;
    Camera camera;

    Sphere add_sphere(const float3& center, float radius, const RendererMaterial& material, int primitive_index)
    {
        int material_index = materials.size();

        materials.push_back(material);
        material_indices.push_back(material_index);

        Sphere sphere(center, radius, primitive_index);

        return sphere;
    }

    std::vector<Triangle> get_triangles()
    {
        std::vector<Triangle> triangles;

        for (int i = 0; i < triangle_indices.size(); i += 3)
        {
            triangles.push_back(Triangle(*reinterpret_cast<float3*>(&vertices_positions[triangle_indices[i + 0]]),
                                         *reinterpret_cast<float3*>(&vertices_positions[triangle_indices[i + 1]]),
                                         *reinterpret_cast<float3*>(&vertices_positions[triangle_indices[i + 2]])));
        }

        return triangles;
    }
};

class SceneParser
{
public:
    /**
     * Parses the scene file at @filepath and returns a scene appropriate for the renderer.
     * All formats supported by the ASSIMP library are supported by the renderer
     * 
     * If provided, the @frame_aspect_override parameter is meant to override the aspect ratio of the camera
     * of the scene file (if any). This is useful because the renderer uses a default aspect ratio
     * of 16:9 but the camera of the scene file ma not use the same aspect. Without this parameter,
     * this would result in rendering the scene with an aspect different of 16:9 in the defualt 
     * framebuffer of the renderer which is 16:9, resulting in deformations.
     */
    static Scene parse_scene_file(const std::string& filepath, float frame_aspect_override = -1.0f);

private:

    static void parse_camera(const aiScene* scene, Scene& parsed_scene, float frame_aspect_override);
    /** 
     * Prepares all the necessary data for multithreaded texture-loading
     */
    static void prepare_textures(const aiScene* scene, std::vector<std::pair<aiTextureType, std::string>>& texture_paths, std::vector<ParsedMaterialTextureIndices>& material_texture_indices, std::vector<int>& material_indices, std::vector<int>& texture_per_mesh, std::vector<int>& texture_indices_offsets, int& texture_count);
    static void assign_material_texture_indices(std::vector<RendererMaterial>& materials, const std::vector<ParsedMaterialTextureIndices>& material_tex_indices, const std::vector<int>& material_textures_offsets);
    static void dispatch_texture_loading(std::vector<std::thread>& threads, Scene& parsed_scene, const std::string& scene_path, const std::vector<std::pair<aiTextureType, std::string>>& texture_paths);
    static void thread_load_texture(Scene& parsed_scene, const std::string& scene_path, const std::vector<std::pair<aiTextureType, std::string>>& tex_paths, int thread_index, int nb_threads);

    static void read_material_properties(aiMaterial* mesh_material, RendererMaterial& renderer_material);
    /**
     * Check if the mesh material has a texture of the given type. If so, returns the index of the
     * texture within texturePathList and appends the path of the texture to the list. If the material
     * doesn't have the required texture, returns -1
     */
    static int get_first_texture_of_type(aiMaterial* mesh_material, aiTextureType type, std::vector<std::pair<aiTextureType, std::string>>& texture_path_list);
    static std::vector<std::pair<aiTextureType, std::string>> get_textures_paths_and_indices(aiMaterial* mesh_material, ParsedMaterialTextureIndices& texture_indices);
    static std::vector<std::pair<aiTextureType, std::string>> normalize_texture_paths(std::vector<std::pair<aiTextureType, std::string>>& paths);
    static RendererMaterial offset_textures_indices(const RendererMaterial& renderer_material, int offset);
};

#endif
