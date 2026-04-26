# DirectX 11 IBL Diffuse + Specular Application

DirectX 11 application rendering a PBR sphere grid with HDRI-based environment lighting.

## Features

- Loads a Radiance HDRI file (`*.hdr`) from `assets/hdri`
- Converts the equirectangular HDRI texture into a cubemap texture on the GPU
- Displays the converted cubemap on the environment sphere
- Builds an irradiance cubemap from the environment cubemap
- Builds a prefiltered specular cubemap with roughness encoded in mip levels
- Builds a BRDF integration LUT for split-sum environment specular
- Uses the irradiance map in the PBR shader for ambient diffuse lighting
- Uses the prefiltered cubemap and BRDF LUT in the PBR shader for ambient specular reflections
- Keeps three point lights for direct lighting and allows toggling them with the `L` key
- Preserves the PBR debug modes for NDF, Geometry, and Fresnel with digit keys `1..4`

## HDRI File

Place exactly one HDR file into:

- `pbr-ibl-specular/assets/hdri`

Recommended file requirements:

- Format: Radiance HDR (`*.hdr`)
- Projection: equirectangular / lat-long
- Aspect ratio: `2:1`
- Recommended resolution: `2048x1024` or `4096x2048`
