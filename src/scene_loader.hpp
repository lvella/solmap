#pragma once

#include <memory>
#include <assimp/Importer.hpp>

std::unique_ptr<const Assimp::Importer> load_scene(const char* filename);
