# Garry's Mod DXR On

Binary module that renders the GMod environment almost entirely directly using DirectX Raytracing via NVIDIA's [Falcor framework](https://developer.nvidia.com/falcor).  

Uses PNG textures placed in `GarrysMod/bin/win64/Data/Overrides/materials` with matching paths to the relative ones in an entity's VMT file (currently `$basetexture` and `$bumpmap`), and will hopefully eventually load texture data on the fly from mounted VPKs, GMAs, and filesystem addons (with the overrides folder being used for selective texture replacement, which would mainly be used for PBR textures like occlusion, metalness, and roughness).  

The iterative path tracer currently handles diffuse and specular lobes of the Falcor BSDF, as well as direct lighting using analytic lights and emissives and envmap sampling with MIS (multiple importance sampling).  

The accumulator and tonemapper shaders are modified versions of those found in Falcor's [accumulation](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/RenderPasses/AccumulatePass) and [tonemapping](https://github.com/NVIDIAGameWorks/Falcor/tree/master/Source/RenderPasses/ToneMapper) render passes, using compensated accumulation, and ACES tonemapping with auto exposure and toggleable white balance adjustment.  

![Example Render](https://github.com/Derpius/gmod-dxr/blob/master/Screenshots/PBR%20Textures.png)  
![Example Render](https://github.com/Derpius/gmod-dxr/blob/master/Screenshots/ragdolls.png)  

PBR textures are made by me using Quixel Mixer and GIMP, you can get them [here](https://github.com/Derpius/gmod-dxr-pbr).  

Map surfaces will be broken until I implement loading BSPs myself, as GMod's [SurfaceInfo](https://wiki.facepunch.com/gmod/SurfaceInfo) classes are missing key faces (although unlikely, this may be due to some SurfaceInfos being present on entities other than world, which I have yet to test).
