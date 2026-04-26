# DirectX 11 glTF Metallic-Roughness Viewer

DirectX 11 application that loads a glTF 2.0 model and renders it with HDRI-based image-based lighting and a metallic-roughness PBR shader.

## Features

- Loads the first `*.gltf` or `*.glb` file found in `assets/models`
- Supports glTF 2.0 scenes with mesh nodes, transforms, indices, and triangle primitives
- Uses only the metallic-roughness material workflow
- Reads `baseColorFactor`, `metallicFactor`, `roughnessFactor`, `emissiveFactor`, and `doubleSided`
- Loads embedded or external glTF textures for base color, metallic-roughness, emissive, and occlusion inputs
- Builds the environment cubemap, irradiance map, prefiltered specular map, and BRDF LUT from the HDRI
- Keeps the BRDF debug modes from the previous PBR demo
- Includes a sample metallic torus model in `assets/models/metallic_torus.gltf`

## Supported glTF Subset

- Formats: `*.gltf`, `*.glb`
- Primitive mode: triangles only
- Vertex attributes: `POSITION`, `NORMAL`, `TEXCOORD_0`
- Buffers: external files or base64 data URIs
- Materials: core metallic-roughness only

Not supported:

- `KHR_materials_pbrSpecularGlossiness`
- skinning, animation, morph targets, sparse accessors

## Assets

Place assets here:

- HDRI: `gltf-metallic-roughness/assets/hdri`
- glTF model: `gltf-metallic-roughness/assets/models`

The project will automatically pick the first matching file from each folder.

## Controls

- `LMB` drag: orbit camera
- Mouse wheel: zoom
- `R`: reset camera
- `L`: toggle point lights
- `1`: full PBR
- `2`: normal distribution debug
- `3`: geometry term debug
- `4`: fresnel debug
- `5`: direct lighting only
