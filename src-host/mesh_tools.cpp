#include <unordered_map>
#include <stdexcept>
#include <cmath>

#include <boost/functional/hash.hpp>

#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include "mesh_tools.hpp"

// Finding the axis aligned bounding box.
static real bounding_box(const Mesh& m, Vec3& center)
{
	Vec3 lo = m.vertices[0].position;
	Vec3 hi = lo;

	for(size_t i = 1; i < m.vertices.size(); ++i) {
		const Vec3 &p = m.vertices[i].position;
		for(size_t j = 0; j < 3; ++j) {
			if(p[j] < lo[j]) {
				lo[j] = p[j];
			} else if(p[j] > hi[j]) {
				hi[j] = p[j];
			}
		}
	}
	center = (lo + hi) * 0.5f;
	return glm::distance(center, hi);
}

// Code adapted from assimp library example
// http://sir-kimmi.de/assimp/lib_html/usage.html
static Mesh import_scene_from_file(const std::string& filename)
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
		aiProcess_PreTransformVertices |
		//aiProcess_ValidateDataStructure |
		aiProcess_ImproveCacheLocality |
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
	for(size_t i = 0; i < scene->mNumMeshes; ++i) {
		auto* m = scene->mMeshes[i];
		if(!m->mNormals) {
			throw std::runtime_error(
				"Missing normals on mesh.\n"
			);
		}

		for(size_t j = 0; j < m->mNumVertices; ++j) {
			ret.vertices.emplace_back();
			VertexData& v = ret.vertices.back();

			for(uint8_t k = 0; k < 3; ++k) {
				v.position[k] = m->mVertices[j][k];
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

Mesh load_scene(const std::string& filename, const Quat& rotation, real &scale)
{
	Mesh ret = import_scene_from_file(filename);

	// Find the bounding sphere of the point set:
	Vec3 center;

	// Find the bounding box to scale to the rendering buffer.
	const real radius = bounding_box(ret, center);

	// Transform mesh. Scale so radius is 1, thus it
	// always fit in the rendered buffer, no matter what
	// rotation is applied. Also applies the mesh rotation,
	// so it doesn't need to be done on GPU for every
	// rendered frame.
	for(auto& v: ret.vertices) {
		v.position = glm::rotate(rotation, v.position - center)
			/ radius;
		v.normal = glm::rotate(rotation, v.normal);
	}

	// Update the user defined scale, so it applies to
	// the newly normalized coordiates.
	scale *= radius;

	return ret;
}

struct OrderedPair
{
	OrderedPair(uint32_t a, uint32_t b)
	{
		if(a < b) {
			first = a;
			second = b;
		} else {
			first = b;
			second = a;
		}
	}

	bool operator==(const OrderedPair& other) const
	{
		return (first == other.first)
			&& (second == other.second);
	}

	uint32_t first;
	uint32_t second;
};

template<>
class std::hash<OrderedPair>
{
public:
	size_t operator()(OrderedPair const& s) const noexcept
	{
		std::size_t seed = 0;
		boost::hash_combine(seed, s.first);
		boost::hash_combine(seed, s.second);

		return seed;
	}
};

class Refiner
{
public:
	Refiner(std::vector<VertexData>& vertices, float max_length):
		vs(vertices),
		maxl2(max_length * max_length)
	{}

	bool refine_face(const uint32_t *oidx, std::vector<uint32_t>& output)
	{
		// Copy the original idices because we
		// will replace them inplace.
		struct Indexer {
			uint32_t operator[](uint8_t i)
			{
				return is[(i+s)%3];
			}

			uint32_t is[3];
			uint8_t s = 0;
		} idx;
		std::copy_n(oidx, 3, idx.is);

		// Decide which edges must be refined:
		bool must_refine[3];
		uint8_t ref_count = 0;
		for(uint8_t i = 0; i < 3; ++i) {
			// We use distance squared because it
			// is faster to compute.
			float len2 = glm::distance2(
				vs[idx[i]].position,
			       	vs[idx[i+1]].position
			);

			if(len2 > maxl2) {
				must_refine[i] = true;
				++ref_count;
			} else {
				must_refine[i] = false;
			}
		}

		// Start position so that the refined edges comes first.
		uint32_t nverts[3];
		switch(ref_count) {
		case 0:
			return false;

		case 1:
			// Find the refined edge:
			for(idx.s = 0; !must_refine[idx.s]; ++idx.s);

			// Add the new vertex:
			nverts[0] = refine_edge(idx[0], idx[1]);

			// Replace this face with 2 new faces
			output.push_back(idx[0]);
			output.push_back(nverts[0]);
			output.push_back(idx[2]);

			output.push_back(nverts[0]);
			output.push_back(idx[1]);
			output.push_back(idx[2]);

			break;
		case 2:
			// Find the non-refined edge:
			for(idx.s = 0; must_refine[idx.s]; ++idx.s);

			// Shift to the first refined edge:
			++idx.s;

			// Add the new vertices:
			for(uint8_t i = 0; i < 2; ++i) {
				nverts[i] = refine_edge(
					idx[i], idx[i+1]
				);
			}

			// Replace this face with 3 new faces:

			// First face is between the two divided edges:
			output.push_back(nverts[0]);
			output.push_back(idx[1]);
			output.push_back(nverts[1]);

			// Next we must decide how to split the remaining
			// quadrilateral so that the pair of triangles
			// are closest to regular. We choose to cut through
			// the smallest diagonal.
			if(glm::distance2(vs[idx[0]].position,
				vs[nverts[1]].position) <
				glm::distance2(vs[idx[2]].position,
				vs[nverts[0]].position))
			{
				output.push_back(idx[0]);
				output.push_back(nverts[1]);
				output.push_back(idx[2]);

				output.push_back(idx[0]);
				output.push_back(nverts[0]);
				output.push_back(nverts[1]);
			} else {
				output.push_back(idx[0]);
				output.push_back(nverts[0]);
				output.push_back(idx[2]);

				output.push_back(nverts[0]);
				output.push_back(nverts[1]);
				output.push_back(idx[2]);
			}

			break;
		case 3:
			// Add the new vertices:
			for(uint8_t i = 0; i < 3; ++i) {
				nverts[i] = refine_edge(
					idx[i], idx[i+1]
				);
			}

			// All edges are refined, replace this
			// face with four others.
			output.push_back(idx[0]);
			output.push_back(nverts[0]);
			output.push_back(nverts[2]);

			output.push_back(nverts[0]);
			output.push_back(idx[1]);
			output.push_back(nverts[1]);

			output.push_back(nverts[1]);
			output.push_back(idx[2]);
			output.push_back(nverts[2]);

			output.push_back(nverts[0]);
			output.push_back(nverts[1]);
			output.push_back(nverts[2]);

			break;
		}

		return true;
	}

	uint32_t refine_edge(uint32_t a, uint32_t b)
	{
		const OrderedPair key{a, b};
		auto ret = ref_edge.try_emplace(key);

		// Edge already refined, return the middle vertex:
		if(!ret.second) {
			return ret.first->second;
		}

		// Add a new vertex at the middle
		auto& va = vs[key.first];
		auto& vb = vs[key.second];

		vs.emplace_back(
			(va.position + vb.position)*0.5f,
			glm::normalize((va.normal + vb.normal)*0.5f)
		);
		return ret.first->second = (vs.size() - 1);
	}

	void clear()
	{
		ref_edge.clear();
	}

private:
	std::vector<VertexData>& vs;
	float maxl2;

	std::unordered_map<OrderedPair, uint32_t> ref_edge;
};

void refine(Mesh& m, float max_length)
{
	Refiner ref(m.vertices, max_length);

	std::vector<uint32_t> final_idx;

	std::vector<uint32_t> input = std::move(m.indices);
	std::vector<uint32_t> output;

	// Refine all faces iterativelly, until every edge
	// is at most max_length.
	while(!input.empty()) {
		// Go through every face and refine them:
		const size_t num_faces = input.size() / 3;
		for(size_t i = 0; i < num_faces; ++i)
		{
			if(!ref.refine_face(&input[i*3], output)) {
				final_idx.insert(final_idx.end(),
					input.begin() + (i*3),
					input.begin() + (i*3 + 3)
				);
			}
		}
		ref.clear();
		input = std::move(output);
		output.clear();
	};

	m.indices = std::move(final_idx);
}
