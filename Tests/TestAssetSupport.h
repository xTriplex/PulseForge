#pragma once

#include "TestSupport.h"

#include "Core/Log.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/MaterialAssetService.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/PrefabSerializer.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/UUID.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
