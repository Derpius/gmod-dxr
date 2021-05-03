# Garry's Mod DXR On

*Not sure why the Falcor submodule isn't pushing, it's in `.gitmodules` and shows as a submodule in vscode*

Decided to use [NVIDIA Falcor](https://developer.nvidia.com/falcor) as a nice abstraction layer over the extremely low level DirectX 12 API (well less a layer and more like an entire heavily moddable engine).  

So far I have a functional method for loading and unloading the Falcor renderer using surface infos for scene geometry, without blocking the gmod process and piping in the required data from the GLua side, and can edit the debug ray tracing slang shader file without restarting gmod by just closing the Falcor window and reloading the GLua script.  

This is what I've done so far, just some *very* basic dot shading using the sun direction and hit normal:  
![Example Render](https://github.com/100PXSquared/gmod-dxr/blob/master/Screenshots/first%20operational%20example.png)  

As you can see quite a few issues still, huge swathes of the map aren't brushes in the worldspawn entity, so they don't get piped across, no displacements, no texturing whatsoever, and the entire map is mirrored due to not swapping the correct vector components to go from the coordinate system Source uses, to the one Falcor uses (mainly for the camera controllers).
