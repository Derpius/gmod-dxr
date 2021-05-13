# Garry's Mod DXR On

Decided to use [NVIDIA Falcor](https://developer.nvidia.com/falcor) as a nice abstraction layer over the extremely low level DirectX 12 API (well less a layer and more like an entire heavily moddable engine).  

So far I have a functional method for loading and unloading the Falcor renderer using surface infos for map geometry and importing props using textures extracted from vpks and converted to pngs in the `Data/Overrides` folder (subfolder created manually), without blocking the gmod process and piping in the required data from the GLua side, and can edit the debug ray tracing slang shader file without restarting gmod by just closing the Falcor window and reloading the GLua script.  

Fullbright scene showing textured entities:  
![Example Render](https://github.com/100PXSquared/gmod-dxr/blob/master/Screenshots/textured%20entities.png)  

Map surfaces will be broken until I implement loading BSPs myself.
