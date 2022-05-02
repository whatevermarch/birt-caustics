project (BIRT_VK)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../common.cmake)

set(headers
	pch.h
	Renderer.h
	DirectLighting.h
	IndirectLighting.h
	Aggregator.h
	Caustics.h
	SVGF.h
	Fresnel.h
	ISRTCommon.h
	CausticsMapping.h
	Ocean.h)
source_group("Header Files" FILES ${headers})

set(sources
	App.cpp
	Renderer.cpp
	DirectLighting.cpp
	IndirectLighting.cpp
	Aggregator.cpp
	Caustics.cpp
	SVGF.cpp
	Fresnel.cpp
	CausticsMapping.cpp
	Ocean.cpp)
source_group("Source Files" FILES ${sources})

set(shaders
	PBRLighting.h
	Bypass-vert.glsl
	DirectLighting-frag.glsl
	IndirectLighting-frag.glsl
	Aggregator.glsl
	RGBEConversion.h
	PhotonTracer.glsl
	SVGFEdgeStoppingFunc.h
	SVGFReproject.glsl
	SVGFStabilityBoost.glsl
	SVGFAtrousWT.glsl
	PathTracer.glsl
	ImageSpaceRT.h
	TransformParams.glsl
	CausticsMapping-vert.glsl
	CausticsMapping-frag.glsl
	CausticsMapReproj.glsl
	Ocean-vert.glsl
	Ocean-tess.glsl
	Ocean-frag.glsl)
source_group("Shader Files" FILES ${shaders})
set_source_files_properties(${shaders} PROPERTIES VS_TOOL_OVERRIDE "Text")

add_executable(${PROJECT_NAME} WIN32 ${sources} ${headers} ${shaders}) 
target_link_libraries (${PROJECT_NAME} LINK_PUBLIC Cauldron_VK ImGUI Vulkan::Vulkan)
target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

set_target_properties(${PROJECT_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${shaders} "${CMAKE_HOME_DIRECTORY}/bin/ShaderLibVK"
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

addManifest(${PROJECT_NAME})
