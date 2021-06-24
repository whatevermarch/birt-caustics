#pragma once

#include "DirectLighting.h"
#include "Caustics.h"
//#include "IndirectLighting.h"
#include "Aggregator.h"

class Renderer
{
public:

	struct State
	{
		XMVECTOR sunDir;
		float DIWeight = 0.5f; // 0 = full dLight, 1 = full iLight
	};

	//	mandatory methods
	void OnCreate(Device* pDevice, SwapChain* pSwapChain);
	void OnDestroy();
	void OnCreateWindowSizeDependentResources(SwapChain* pSwapChain, uint32_t Width, uint32_t Height);
	void OnDestroyWindowSizeDependentResources();
	void OnRender(SwapChain* pSwapChain, Camera* pCamera, State* pState);

	int loadScene(GLTFCommon* pLoader, int stage);
	void unloadScene();

protected:

	//	pointer to device
	Device* pDevice = nullptr;

	uint32_t width, height;

	//	viewport & rectangle scissor
	VkViewport viewport;
	VkRect2D rectScissor;

	//  GUI (view component in MVC, 
	//	controller(C) will be managed in frontend App class)
	GUI gui;

	//	resources managers
	StaticBufferPool sBufferPool;	// geometry
	DynamicBufferRing dBufferRing;	// uniform buffers
	ResourceViewHeaps resViewHeaps;	// descriptor sets
	CommandListRing cmdBufferRing;	// command buffers
	UploadHeap uploadHeap;			// staging buffers
	// GPUTimestamps gTimeStamps;
	AsyncPool asyncPool;

	//	resources handles
	GLTFTexturesAndBuffers* res_scene = nullptr;
	
	//	G-Buffer pass
	GBuffer* pGBuffer = nullptr;						
	GBufferRenderPass rp_gBuffer_opaq, rp_gBuffer_trans;
	GltfPbrPass* pGltfPbrPass = nullptr;				

	//	RSM pass
	GBuffer* pRSM = nullptr;
	GBufferRenderPass rp_RSM_opaq, rp_RSM_trans;
	GltfPbrPass* pRSMPass = nullptr;

	//	render target caches
	Texture cache_rsmDepth, cache_gbufDepth;
	VkImageView cache_rsmDepthSRV = VK_NULL_HANDLE,
		cache_gbufDepthSRV = VK_NULL_HANDLE;

	//	caches mipmap
	DownSamplePS cache_rsmDepthMipmap, cache_gbufDepthMipmap;
	
	//	lighting passes
	DirectLighting* dLighting = nullptr;

	//	GI effects
	Caustics* caustics = nullptr;
	//IndirectLighting* iLighting = nullptr;

	//	skydome
	//SkyDome skyDome;
	SkyDomeProc skyDomeProc;
	GBufferRenderPass rp_skyDome;

	//	post-processing handle
	Aggregator aggregator_1; //, aggregator_2;
	ToneMapping toneMapping;
	TAA tAA;
	
	//	ToDo : setup renderpass containing multiple subpasses instead
	void setupRenderPass();

	void barrier_Cache_GO_RO(VkCommandBuffer cmdBuf);
	void barrier_DS(VkCommandBuffer cmdBuf); // future : DS_AO_I1
	void barrier_RT(VkCommandBuffer cmdBuf); // future : RT_I2
	void barrier_D_C(VkCommandBuffer cmdBuf);
	void barrier_A1(VkCommandBuffer cmdBuf);
	void barrier_GT_Cache_D(VkCommandBuffer cmdBuf);
	void barrier_DT_RF(VkCommandBuffer cmdBuf);
	void barrier_A2(VkCommandBuffer cmdBuf);
	void barrier_AA(VkCommandBuffer cmdBuf);
};
