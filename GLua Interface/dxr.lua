if SERVER then return end

PLR = LocalPlayer()

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
	Initialise and launch Falcor application
]]
print("GModDXR: Launching Falcor...")
LaunchFalcor(
	worldVertices,
	#worldVertices,
	PLR:EyePos(),
	PLR:EyePos() + PLR:EyeAngles():Forward(),
	-util.GetSunInfo().direction,
	table.Add(ents.FindByClass("prop_physics"), ents.FindByClass("prop_ragdoll"))
)
