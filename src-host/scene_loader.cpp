// Code adapted from assimp library example
// http://sir-kimmi.de/assimp/lib_html/usage.html

#include <stdexcept>
#include <cmath>

#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>

#include "scene_loader.hpp"

Mesh load_scene(const char* filename)
{
	// Create an instance of the Importer class
	Assimp::Importer importer;

	// Configure the importer to filter out everything but the mesh data.
	importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS,
		aiComponent_TANGENTS_AND_BITANGENTS |
		aiComponent_COLORS |
		aiComponent_TEXCOORDS |
		aiComponent_BONEWEIGHTS |
		aiComponent_ANIMATIONS |
		aiComponent_TEXTURES |
		aiComponent_LIGHTS |
		aiComponent_CAMERAS |
		aiComponent_MATERIALS
	);

	// Configure the importer to normalize the scene to [-1, 1] range
	importer.SetPropertyBool(AI_CONFIG_PP_PTV_NORMALIZE, true);

	// Leave only triangles in the mesh (not points nor lines)
	importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
		aiPrimitiveType_POINT |
		aiPrimitiveType_LINE
	);

	// Remove degenerate primitives
	importer.SetPropertyBool(AI_CONFIG_PP_FD_REMOVE, true);

	// And have it read the given file with some example postprocessing
	// Usually - if speed is not the most important aspect for you - you'll
	// probably to request more postprocessing than we do in this example.
	if(!importer.ReadFile(filename,
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_SortByPType |
		aiProcess_RemoveComponent |
		aiProcess_GenSmoothNormals |
		aiProcess_PreTransformVertices |
		//aiProcess_ValidateDataStructure |
		aiProcess_ImproveCacheLocality |
		aiProcess_FixInfacingNormals |
		aiProcess_SortByPType |
		aiProcess_FindDegenerates |
		aiProcess_FindInvalidData
	)) {
		// If the import failed, report it
		throw std::runtime_error(
			"Could not load provided 3D scene file.\n"
		);
	}

	// Now we can access the file's contents.
	// Before returning, we unify all the meshes in a single
	// buffer, to simplify the rendering and computation.
	const aiScene* scene = importer.GetScene();

	// Count before allocating and copying
	size_t vert_count = 0;
	size_t idx_count = 0;
	for(size_t i = 0; i < scene->mNumMeshes; ++i) {
		auto* m = scene->mMeshes[i];
		vert_count += m->mNumVertices;
		idx_count += m->mNumFaces * 3;
	}

	Mesh ret;
	ret.vertices.reserve(vert_count);
	ret.indices.reserve(idx_count);


	// Copy the vertex data.
	// In the process, scale it so it will always stay
	// within the render area. Since we know all the points
	// are within [-1, 1], the biggest lenth possible is 2*sqrt(3)
	// (the diagonal of a cube with L = 2). Thus, by scalig by
	// 1/sqrt(3), we ensure que biggest lenght fits in the [-1, 1]
	// square of the render area.
	const real factor = 1.0 / std::sqrt(3.0);
	for(size_t i = 0; i < scene->mNumMeshes; ++i) {
		auto* m = scene->mMeshes[i];
		for(size_t j = 0; j < m->mNumVertices; ++j) {
			ret.vertices.emplace_back();
			VertexData& v = ret.vertices.back();

			for(uint8_t k = 0; k < 3; ++k) {
				v.position[k] = factor * m->mVertices[j][k];
				v.normal[k] = m->mNormals[j][k];
			}
		}

		for(size_t j = 0; j < m->mNumFaces; ++j) {
			for(uint8_t k = 0; k < 3; ++k) {
				ret.indices.push_back(m->mFaces[j].mIndices[k]);
			}
		}
	}

	return ret;
}
