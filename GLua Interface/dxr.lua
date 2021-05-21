if SERVER then return end

PLR = LocalPlayer()

local unitMatrix = {
	{1, 0, 0, 0},
	{0, 1, 0, 0},
	{0, 0, 1, 0},
	{0, 0, 0, 1},
}

local function GModMatrixToCPP(matrix)
	local ret = {
		{1, 0, 0, 0},
		{0, 1, 0, 0},
		{0, 0, 1, 0},
		{0, 0, 0, 1},
	}

	local nonZero = false
	for row = 1, 4 do
		for col = 1, 4 do
			local cell = matrix:GetField(row, col)
			if cell ~= 0 then nonZero = true end

			ret[row][col] = cell
		end
	end

	if not nonZero then ret = unitMatrix end
	return ret
end

print("=====LOADING GARRY'S MOD DIRECTX RAYTRACING=====")
require("GModDXR") -- Import the binary module
if not LaunchFalcor then error("GModDXR: Failed to import C functions") end

--[[
	World
]]
local worldVertices = {}

-- Build meshes from world
print("GModDXR: Triangulating world...")
local function TriangulateSurface(surface)
	for i = 3, #surface do
		local len = #worldVertices
		worldVertices[len + 1] = surface[1]
		worldVertices[len + 2] = surface[i - 1]
		worldVertices[len + 3] = surface[i]
	end
end

local surfaces = game.GetWorld():GetBrushSurfaces()
if surfaces then
	for i = 1, #surfaces do
		if not surfaces[i]:IsNoDraw() and not surfaces[i]:IsSky() then
			TriangulateSurface(surfaces[i]:GetVertices())
		end
	end
end


--[[
	Entities
]]
local entities = {}
for _, v in pairs(ents.FindByClass("prop_physics")) do
	local meshes = util.GetModelMeshes(v:GetModel())
	if meshes then
		for i = 1, #meshes do
			local subMat = Material(v:GetMaterial() ~= "" and v:GetMaterial() or (v:GetSubMaterial(i - 1) == "" and v:GetMaterials()[i] or v:GetSubMaterial(i - 1)))

			entities[#entities + 1] = {
				name = "Prop",
				model = meshes[i].triangles,
				colour = Vector(v:GetColor().r / 255, v:GetColor().g / 255, v:GetColor().b / 255),
				alpha = v:GetColor().a / 255,
				transform = GModMatrixToCPP(v:GetWorldTransformMatrix()),
				baseTexture = subMat:GetString("$basetexture") and subMat:GetString("$basetexture")..".png" or "missingtexture.png",
				normalTexture = subMat:GetString("$bumpmap") and subMat:GetString("$bumpmap")..".png" or ""
			}
		end
	end
end

--[[
	Initialise and launch Falcor application
]]
print("GModDXR: Launching Falcor...")
LaunchFalcor(
	worldVertices,
	#worldVertices,
	PLR:EyePos(),
	PLR:EyePos() + PLR:EyeAngles():Forward(),
	-util.GetSunInfo().direction,
	entities
)