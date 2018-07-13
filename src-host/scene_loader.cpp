// Code adapted from assimp library example
// http://sir-kimmi.de/assimp/lib_html/usage.html

#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "scene_loader.hpp"

std::unique_ptr<const Assimp::Importer> load_scene(const char* filename)
{
	// Create an instance of the Importer class
	auto importer = std::make_unique<Assimp::Importer>();

	// Configure the importer to filter out everything but the mesh data.
	importer->SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS,
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
	// TODO: do normalization inside GPU and remove this.
	importer->SetPropertyBool(AI_CONFIG_PP_PTV_NORMALIZE, true);

	// Leave only triangles in the mesh (not points nor lines)
	importer->SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
		aiPrimitiveType_POINT |
		aiPrimitiveType_LINE
	);

	// Remove degenerate primitives
	importer->SetPropertyBool(AI_CONFIG_PP_FD_REMOVE, true);

	// And have it read the given file with some example postprocessing
	// Usually - if speed is not the most important aspect for you - you'll
	// probably to request more postprocessing than we do in this example.
	if(!importer->ReadFile(filename,
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
	// We return the whole importer because taking the ownership
	// of the scene seems to cause problems on Windows.
	return importer;
}
