#pragma once

#include "CausticsMapping.h"

#include "SVGF.h"
#include "ISRTCommon.h"

#define USE_BIRT

class Caustics
{
public:

    struct Constants
    {
        ISRTTransform camera;
        ISRTTransform lights[4]; // we have 4 quarters of RSM

        float samplingMapScale = 2.0f;
        float rayThickness_xy;
        float rayThickness_z;
        float tMax = 100.f;
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pResourceViewHeaps,
        DynamicBufferRing* pDynamicBufferRing,
        GBuffer* pRSM, VkImageView rsmDepthOpaque0SRV,
	    Texture* pRSMDepthOpaque1N, int mipCount, 
        VkRenderPass renderPass = VK_NULL_HANDLE);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(
        uint32_t Width, uint32_t Height,
        GBuffer* pGBuffer, 
        VkImageView gbufDepthOpaque0SRV, Texture* pGBufDepthOpaque1N, int mipCount);
    void OnDestroyWindowSizeDependentResources();

    //  if BIRT is not utilized, these two methods have no effect.
    void registerScene(GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers)
    {
#ifndef USE_BIRT
        this->causticsMap.registerScene(pGLTFTexturesAndBuffers);
#endif
    }
    void deregisterScene()
    {
#ifndef USE_BIRT
        this->causticsMap.deregisterScene();
#endif
    }

    Texture* GetTexture() { return &this->pm_irradianceMap; }
    const Texture* GetTexture() const { return &this->pm_irradianceMap; }
    VkImageView GetTextureView() {return this->pm_irradianceMapSRV; }

	void Draw(VkCommandBuffer commandBuffer, const VkRect2D& renderArea, const Caustics::Constants& constants);

    void setGPUTimeStamps(GPUTimestamps* pGPUTimeStamps)
    { this->pGPUTimeStamps = pGPUTimeStamps; }

protected:

    Device* pDevice = nullptr;

    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    GPUTimestamps* pGPUTimeStamps = nullptr;

    GBuffer* pGBuffer = nullptr;

    uint32_t              rsmWidth = 0, rsmHeight = 0;
    uint32_t              outWidth = 0, outHeight = 0;

    PostProcCS photonTracer;

    int                   mipCount_rsm = 0;
    int                   mipCount_gbuf = 0;

    //  photon tracing stuff
    //
    Texture               samplingMap;
    VkImageView           samplingMapSRV = VK_NULL_HANDLE;
    int                   samplingSeed = 0;

    VkImageView           rsmDepthOpaque1NSRV = VK_NULL_HANDLE;
    VkImageView           gbufDepthOpaque1NSRV = VK_NULL_HANDLE;

    StaticBufferPool      hitpointBuffer;
    VkDescriptorBufferInfo hitPosDescInfo;
    VkDescriptorBufferInfo hitDirDescInfo;

    VkSampler             sampler_default = VK_NULL_HANDLE;
    VkSampler             sampler_depth = VK_NULL_HANDLE;
    VkSampler             sampler_noise = VK_NULL_HANDLE;

    VkDescriptorSet       descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    void createPhotonTracerDescriptors(DefineList* pDefines);

    //  photon mapping (point renderer) stuff
    //
    Texture               pm_irradianceMap;
    VkImageView           pm_irradianceMapSRV = VK_NULL_HANDLE;

    VkPipeline            pm_pipeline;
    VkPipelineLayout      pm_pipelineLayout;

    VkRenderPass          pm_renderPass;

    VkFramebuffer         pm_framebuffer;

    void createPhotonMapperPipeline(const DefineList& defines);

    //  denoiser
    SVGF denoiser;

    void generateSamplingPoints(UploadHeap& uploadHeap);

    //  alternate algorithm for caustics
    //  WARNING : just to evaluete ONLY!!
#ifndef USE_BIRT
    CausticsMapping       causticsMap;
#endif
};

