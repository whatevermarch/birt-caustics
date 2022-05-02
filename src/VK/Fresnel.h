#pragma once

#include "SVGF.h"
#include "ISRTCommon.h"

class Fresnel
{
public:

    struct Constants
    {
        ISRTTransform camera;
        
        float samplingMapScale = 2.0f;
        float IOR;
        float rayThickness;
        float tMax = 100.f;
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pResourceViewHeaps,
        DynamicBufferRing* pDynamicBufferRing,
        VkRenderPass renderPass = VK_NULL_HANDLE);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(
        uint32_t Width, uint32_t Height,
        GBuffer* pGBuffer,
        VkImageView gbufDepthOpaque0SRV, 
        Texture* pGBufDepthOpaque1N,
        VkImageView opaqueHDRSRV);
    void OnDestroyWindowSizeDependentResources();

    void Draw(VkCommandBuffer commandBuffer, 
        const VkRect2D& renderArea, 
        const Fresnel::Constants& constants);

    Texture* GetTexture() { return &this->radianceMap; }
    const Texture* GetTexture() const { return &this->radianceMap; }
    VkImageView GetTextureView() { return this->radianceMapSRV; }

protected:

    Device* pDevice = nullptr;

    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    GBuffer* pGBuffer = nullptr;

    uint32_t              outWidth = 0, outHeight = 0;

    Texture               samplingMap;
    VkImageView           samplingMapSRV = VK_NULL_HANDLE;
    int                   samplingSeed = 0;

    VkImageView           gbufDepthOpaque1NSRV = VK_NULL_HANDLE;

    Texture               radianceMap;
    VkImageView           radianceMapSRV = VK_NULL_HANDLE;

    VkSampler             sampler_default = VK_NULL_HANDLE;
    VkSampler             sampler_depth = VK_NULL_HANDLE;
    VkSampler             sampler_noise = VK_NULL_HANDLE;

    PostProcCS pathTracer;

    VkDescriptorSet       descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    void createDescriptors(DefineList& defines);

    //  denoiser
    SVGF denoiser;

    void generateSamplingPoints(UploadHeap& uploadHeap);

    void barrier_In(VkCommandBuffer cmdBuf);
    void barrier_Out(VkCommandBuffer cmdBuf);
};