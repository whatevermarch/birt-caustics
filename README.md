# birt-caustics
Bidirectional Image-space Ray Tracing for Real-time Caustics

![teaser](https://user-images.githubusercontent.com/14962580/166307614-b9014613-528f-439b-b7ee-574e534724c8.png)

## How to build
These software are needed.

- CMake 3.16
- Visual Studio 2022
- Windows 10 SDK 10.0.18362
- Vulkan SDK 1.3.204.0

Also, all the submodules are needed to be pulled before building.

Next, create a blank directory `build`, enter that directory, and type `cmake ..`
The Visual Studio solution should be created inside the `build` directory. Open it and compile.
