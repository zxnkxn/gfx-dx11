# DirectX 11 HDR Scene Application

DirectX 11 application rendering an HDR-lit 3D scene with tone mapping and eye adaptation.

## Features

- Camera navigation with mouse orbit and WASD / arrow key target movement
- Three point lights with overlapping lighting contribution
- Light intensity switching between 1, 10, and 100
- HDR scene rendering into a floating-point render target
- Average luminance computation using texture downsampling
- Exposure control and Uncharted2 tone mapping for HDR to LDR conversion
- Eye adaptation based on previous and current scene luminance
