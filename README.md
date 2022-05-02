# birt-caustics
Bidirectional Image-space Ray Tracing for Real-time Caustics

## How to build
These software are needed.

- CMake 3.16
- Visual Studio 2022
- Windows 10 SDK 10.0.18362
- Vulkan SDK 1.3.204.0

Also, all the submodules are needed to be pulled before building.

Next, create a blank directory `build`, enter that directory, and type `cmake ..`
The Visual Studio solution should be created inside the `build` directory. Open it and compile.
