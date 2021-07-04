#include "Renderer.h"
#include "GarrysMod/Lua/Interface.h"

Falcor::float3 gmodToGLMVec(Vector vec) { return Falcor::float3(vec.x, vec.z, -vec.y); }
std::vector<Falcor::float3> computeBrushNormals(const Falcor::float3* positions, const size_t count)
{
	std::vector<Falcor::float3> normals;
	// Iterate tris
	for (size_t i = 0; i < count; i += 3) {
		// Compute normalised cross product of v0 and v1 localised to v2 and appened to normals vector
		// (may need to invert the normal here depending on what winding my triangulation code spits out)
		Falcor::float3 normal = -glm::normalize(glm::cross(positions[i] - positions[i + 2], positions[i + 1] - positions[i + 2]));
		normals.push_back(normal);
		normals.push_back(normal);
		normals.push_back(normal);
	}
	return normals;
}

static std::thread mainThread;
static std::mutex mut;
static GModDXR::WorldData worldData;
static bool TRACING = false;
void falcorThreadWrapper(
	const Vector camPos, const Vector camTarget,
	std::vector<Falcor::TriangleMesh::SharedPtr> meshes, std::vector<Falcor::Material::SharedPtr> materials, std::vector<Falcor::SceneBuilder::Node> nodes, std::vector<GModDXR::TextureList> textures
) {
	// Create renderer
	GModDXR::Renderer::UniquePtr pRenderer = std::make_unique<GModDXR::Renderer>();

	// Get raw casted pointer to GModDXR::Renderer to access custom methods in the derived class
	GModDXR::Renderer* pRendererRaw = reinterpret_cast<GModDXR::Renderer*>(pRenderer.get());
	pRendererRaw->setWorldData(&worldData);
	pRendererRaw->setCameraDefaults(gmodToGLMVec(camPos), gmodToGLMVec(camTarget));
	pRendererRaw->setEntities(&meshes, &materials, &nodes, &textures);
	pRendererRaw = nullptr;

	// Create window config
	Falcor::SampleConfig config;
	config.windowDesc.title = "Garry's Mod DXR";
	config.windowDesc.resizableWindow = true;

	Falcor::Sample::run(config, pRenderer);

	// Let the main thread know we're done
	mut.lock();
	TRACING = false;
	mut.unlock();
}

void printLua(GarrysMod::Lua::ILuaBase* inst, const char text[])
{
	inst->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	inst->GetField(-1, "print");
	inst->PushString(text);
	inst->Call(1, 0);
	inst->Pop();
}

void dumpStack(GarrysMod::Lua::ILuaBase* inst)
{
	using namespace GarrysMod::Lua;
	std::string toPrint = "";

	int max = inst->Top();
	for (int i = 1; i <= max; i++) {
		toPrint += "[" + std::to_string(i) + "] ";
		switch (inst->GetType(i)) {
		case Type::Angle:
			toPrint += "Angle: (" + std::to_string((int)inst->GetAngle(i).x) + ", " + std::to_string((int)inst->GetAngle(i).y) + ", " + std::to_string((int)inst->GetAngle(i).z) + ")";
			break;
		case Type::Bool:
			toPrint += "Bool: " + inst->GetBool(i);
			break;
		case Type::Function:
			toPrint += "Function";
			break;
		case Type::Nil:
			toPrint += "nil";
			break;
		case Type::Number:
			toPrint += "Number: " + std::to_string(inst->GetNumber(i));
			break;
		case Type::String:
			toPrint += "String: " + (std::string)inst->GetString(i);
			break;
		case Type::Table:
			toPrint += "Table";
			break;
		case Type::Entity:
			toPrint += "Entity";
			break;
		default:
			toPrint += "Unknown";
			break;
		}
		toPrint += "\n";
	}

	printLua(inst, toPrint.c_str());
}

// Skins a vertex to its bones
Falcor::float3 transformToBone(
	const Vector& vec,
	const std::vector<glm::mat4>& bones, const std::vector<glm::mat4>& binds,
	const std::vector<std::pair<size_t, float>>& weights,
	const bool angleOnly = false
)
{
	glm::vec4 final(0.f);
	for (size_t i = 0U; i < weights.size(); i++) {
		final += bones[weights[i].first] * binds[weights[i].first] * glm::vec4(vec.x, vec.z, -vec.y, angleOnly ? 0.f : 1.f) * weights[i].second;
	}
	return Falcor::float3(final.x, final.y, final.z);
}

// For transformations between Source and Falcor coordinate systems
static const glm::mat4 zToYUp = glm::mat4(
	Falcor::float4(1, 0, 0, 0),
	Falcor::float4(0, 0, -1, 0),
	Falcor::float4(0, 1, 0, 0),
	Falcor::float4(0, 0, 0, 1)
);
static const glm::mat4 zToYUpTranspose = glm::transpose(zToYUp);

// Gets the string value at a key in the material at the top of the stack
std::string getMaterialString(GarrysMod::Lua::ILuaBase* LUA, const std::string key)
{
	std::string val = "";
	LUA->GetField(-1, "GetString");
	LUA->Push(-2);
	LUA->PushString(key.c_str());
	LUA->Call(2, 1);
	if (LUA->IsType(-1, GarrysMod::Lua::Type::String)) val = LUA->GetString();
	LUA->Pop();

	return val;
}

/*
	Entrypoint for the application when loaded from GLua
	
	Parameters
	- table<Vector> World surface positions
	- number        Number of world vertices
	- Vector        Camera start location
	- Vector        Camera up vector
	- Vector        Sun direction
	- table<Entity> Table of entities
*/
LUA_FUNCTION(LaunchFalcor)
{
	using namespace GarrysMod::Lua;
	if (TRACING) return 0;

	auto meshes = std::vector<Falcor::TriangleMesh::SharedPtr>();
	auto materials = std::vector<Falcor::Material::SharedPtr>();
	auto nodes = std::vector<Falcor::SceneBuilder::Node>();
	auto textures = std::vector<GModDXR::TextureList>();

	// Iterate over entities
	size_t numEntities = LUA->ObjLen(6);
	for (size_t entIndex = 1; entIndex <= numEntities; entIndex++) {
		// Get entity
		LUA->PushNumber(entIndex);
		LUA->GetTable(6);
		LUA->CheckType(-1, Type::Entity);

		// Make sure entity is valid
		LUA->GetField(-1, "IsValid");
		LUA->Push(-2);
		LUA->Call(1, 1);
		if (!LUA->GetBool()) LUA->ThrowError("Attempted to launch Falcor with an invalid entity");
		LUA->Pop(); // Pop the bool

		// Cache bone transforms
		// Make sure the bone transforms are updated and the bones themselves are valid
		LUA->GetField(-1, "SetupBones");
		LUA->Push(-2);
		LUA->Call(1, 0);

		// Get number of bones and make sure the value is valid
		LUA->GetField(-1, "GetBoneCount");
		LUA->Push(-2);
		LUA->Call(1, 1);
		int numBones = LUA->CheckNumber();
		LUA->Pop();
		if (numBones < 1) LUA->ThrowError("Entity has invalid bones");

		// For each bone, cache the transform
		auto bones = std::vector<glm::mat4>(numBones);
		for (int boneIndex = 0; boneIndex < numBones; boneIndex++) {
			LUA->GetField(-1, "GetBoneMatrix");
			LUA->Push(-2);
			LUA->PushNumber(boneIndex);
			LUA->Call(2, 1);

			glm::mat4 transform = glm::identity<glm::mat4>();
			if (LUA->IsType(-1, Type::Matrix)) {
				for (unsigned char row = 0; row < 4; row++) {
					for (unsigned char col = 0; col < 4; col++) {
						LUA->GetField(-1, "GetField");
						LUA->Push(-2);
						LUA->PushNumber(row + 1);
						LUA->PushNumber(col + 1);
						LUA->Call(3, 1);

						transform[col][row] = LUA->CheckNumber();
						LUA->Pop();
					}
				}
				transform = (zToYUp * transform) * zToYUpTranspose;
			}
			LUA->Pop();

			bones[boneIndex] = transform;
		}

		// Iterate over meshes
		LUA->PushSpecial(SPECIAL_GLOB);
		LUA->GetField(-1, "util");
		LUA->GetField(-1, "GetModelMeshes");
		LUA->GetField(-4, "GetModel");
		LUA->Push(-5);
		LUA->Call(1, 1);
		const std::string modelName = LUA->CheckString();
		LUA->Call(1, 2);

		// Make sure both return values are present and valid
		if (!LUA->IsType(-2, Type::Table)) LUA->ThrowError("Entity model invalid");
		if (!LUA->IsType(-1, Type::Table)) LUA->ThrowError("Entity model valid, but bind pose not returned (this likely means you're running an older version of GMod)");

		// Cache bind pose
		auto bindBones = std::vector<glm::mat4>(numBones);
		for (int boneIndex = 0; boneIndex < numBones; boneIndex++) {
			LUA->PushNumber(boneIndex);
			LUA->GetTable(-2);
			LUA->GetField(-1, "matrix");

			glm::mat4 transform = glm::identity<glm::mat4>();
			if (LUA->IsType(-1, Type::Matrix)) {
				for (unsigned char row = 0; row < 4; row++) {
					for (unsigned char col = 0; col < 4; col++) {
						LUA->GetField(-1, "GetField");
						LUA->Push(-2);
						LUA->PushNumber(row + 1);
						LUA->PushNumber(col + 1);
						LUA->Call(3, 1);

						transform[col][row] = LUA->CheckNumber();
						LUA->Pop();
					}
				}
				transform = (zToYUp * transform) * zToYUpTranspose;
			}
			LUA->Pop(2);

			bindBones[boneIndex] = transform;
		}
		LUA->Pop();

		size_t numSubmeshes = LUA->ObjLen();
		for (size_t meshIndex = 1; meshIndex <= numSubmeshes; meshIndex++) {
			// Create empty mesh and material
			Falcor::TriangleMesh::SharedPtr pMesh = Falcor::TriangleMesh::create();
			pMesh->setName(modelName);

			Falcor::Material::SharedPtr pMaterial = Falcor::Material::create("Entity");
			pMaterial->setShadingModel(ShadingModelMetalRough);
			pMaterial->setRoughness(1.f); // Placeholder
			pMaterial->setMetallic(0.f);  // Placeholder

			// Get mesh
			LUA->PushNumber(meshIndex);
			LUA->GetTable(-2);

			// Iterate over tris
			LUA->GetField(-1, "triangles");
			LUA->CheckType(-1, Type::Table);
			size_t numVerts = LUA->ObjLen();
			if (numVerts % 3U != 0U) LUA->ThrowError("Number of triangles is not a multiple of 3");

			for (size_t vertIndex = 0; vertIndex < numVerts; vertIndex++) {
				// Get vertex
				LUA->PushNumber(vertIndex + 1U);
				LUA->GetTable(-2);

				// Get weights
				LUA->GetField(-1, "weights");
				auto weights = std::vector<std::pair<size_t, float>>();
				{
					size_t numWeights = LUA->ObjLen();
					for (size_t weightIndex = 1U; weightIndex <= numWeights; weightIndex++) {
						LUA->PushNumber(weightIndex);
						LUA->GetTable(-2);
						LUA->GetField(-1, "bone");
						LUA->GetField(-2, "weight");
						weights.emplace_back(LUA->CheckNumber(-2), LUA->CheckNumber());
						LUA->Pop(3);
					}
				}
				LUA->Pop();

				// Get and transform position
				LUA->GetField(-1, "pos");
				Falcor::float3 pos = transformToBone(LUA->GetVector(), bones, bindBones, weights);
				LUA->Pop();

				// Get and transform normal
				LUA->GetField(-1, "normal");
				Vector localNormal; // Need to check the normal is actually present in the model mesh (should theoretically always be for game assets, but just in case)
				if (!LUA->IsType(-1, Type::Nil)) {
					localNormal = LUA->GetVector();
				} else {
					localNormal.x = localNormal.y = localNormal.z = 0.f;
				}
				LUA->Pop();
				Falcor::float3 normal = transformToBone(localNormal, bones, bindBones, weights, true);

				// Get uvs
				LUA->GetField(-1, "u");
				LUA->GetField(-2, "v");
				Falcor::float2 uv{ LUA->GetNumber(-2), LUA->GetNumber() };
				LUA->Pop(2);

				// Pop MeshVertex
				LUA->Pop();

				// Add vertex to mesh
				pMesh->addVertex(pos, normal, uv);
				if (vertIndex % 3 == 2) pMesh->addTriangle(vertIndex, vertIndex - 1U, vertIndex - 2U);
			}

			// Pop triangle and mesh tables
			LUA->Pop(2);

			// Get textures
			std::string materialPath = "";
			LUA->GetField(-4, "GetMaterial");
			LUA->Push(-5);
			LUA->Call(1, 1);
			if (LUA->IsType(-1, Type::String)) materialPath = LUA->GetString();
			LUA->Pop();
			if (materialPath == "") {
				LUA->GetField(-4, "GetSubMaterial");
				LUA->Push(-5);
				LUA->PushNumber(meshIndex - 1U);
				LUA->Call(2, 1);
				if (LUA->IsType(-1, Type::String)) materialPath = LUA->GetString();
				LUA->Pop();

				if (materialPath == "") {
					LUA->GetField(-4, "GetMaterials");
					LUA->Push(-5);
					LUA->PushNumber(meshIndex);
					LUA->Call(2, 1);
					LUA->PushNumber(meshIndex);
					LUA->GetTable(-2);
					if (LUA->IsType(-1, Type::String)) materialPath = LUA->GetString();
					LUA->Pop(2); // Path and table
				}
			}

			LUA->GetField(-3, "Material");
			LUA->PushString(materialPath.c_str());
			LUA->Call(1, 1);
			if (LUA->IsType(-1, Type::Nil)) LUA->ThrowError("Invalid material on entity");

			const std::string baseTexture = getMaterialString(LUA, "$basetexture");
			const std::string normalMap = getMaterialString(LUA, "$bumpmap");
			LUA->Pop(); // Pop material object

			textures.push_back(GModDXR::TextureList{ baseTexture + ".png", normalMap + ".png" });

			// Populate material
			LUA->GetField(-4, "GetColor");
			LUA->Push(-5);
			LUA->Call(1, 1);
			Falcor::float4 colour;
			const char fieldNames[5] = "rgba";
			for (unsigned char field = 0; field < 4; field++) {
				const char fieldName[2] = { fieldNames[field], '\0' };
				LUA->GetField(-1, fieldName);
				colour[field] = LUA->GetNumber(-1);
				LUA->Pop();
			}
			LUA->Pop();
			colour /= 255;

			pMaterial->setBaseColor(colour);

			pMaterial->setSpecularTransmission(1.f - colour[3]);
			if (colour[3] < 1.f) pMaterial->setDoubleSided(true); // If the object is transparent, set it to double sided (note that this will only handle baseColour alpha, not transparent textures)
			
			pMaterial->setEmissiveColor(colour);
			pMaterial->setEmissiveFactor(baseTexture == "lights/white" ? 1 : 0);

			pMaterial->setName(baseTexture);

			meshes.emplace_back(pMesh);
			materials.emplace_back(pMaterial);
			nodes.push_back(Falcor::SceneBuilder::Node{ modelName, glm::identity<glm::mat4>(), glm::identity<glm::mat4>() });
		}

		LUA->Pop(4); // Pop meshes, util, and _G tables, and the entity
	}
	LUA->Pop(); // Pop entity table

	// Read camera details and world vert count
	const size_t worldVertCount = LUA->GetNumber(2);
	const Vector camPos = LUA->GetVector(3);
	const Vector camTarget = LUA->GetVector(4);
	const Vector sunDir = LUA->GetVector(5);
	LUA->Pop(4);

	// Read world verts into data structure
	std::vector<Falcor::float3> worldPositions;
	for (size_t i = 0; i < worldVertCount; i++) {
		// Get next vert
		LUA->PushNumber(static_cast<double>(i + 1U));
		LUA->GetTable(1);

		// Add vertex position to world vetices vector as a float3
		worldPositions.push_back(gmodToGLMVec(LUA->GetVector()));
		LUA->Pop();
	}
	LUA->Pop();

	// Create world data
	worldData.length = worldVertCount;
	worldData.pPositions = worldPositions;
	worldData.pNormals = computeBrushNormals(worldPositions.data(), worldVertCount);
	worldData.sunDirection = gmodToGLMVec(sunDir);

	// Run the sample
	TRACING = true;
	mainThread = std::thread(falcorThreadWrapper, camPos, camTarget, meshes, materials, nodes, textures);
	mainThread.detach();
	return 0;
}

GMOD_MODULE_OPEN()
{
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->PushCFunction(LaunchFalcor);
		LUA->SetField(-2, "LaunchFalcor");
	LUA->Pop();

	return 0;
}

GMOD_MODULE_CLOSE()
{
	while (true) {
		mut.lock();
		if (!TRACING) {
			mut.unlock();
			return 0;
		}
		mut.unlock();
	}
}

int main()
{
	return 0;
}
