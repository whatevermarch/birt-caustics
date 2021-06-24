#pragma once

class Caustics
{
public:

    struct Constants
    {
        XMMATRIX camViewProj;
        XMVECTOR camPosition;
        
        float invTanHalfFovH;
        float invTanHalfFovV;

        uint32_t rsmWidth;
        uint32_t rsmHeight;

        XMVECTOR lightPos[4];
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

    Texture* getResult() { return &this->pm_irradianceMap; }
    const Texture* getResult() const { return &this->pm_irradianceMap; }
    VkImageView getResultView() {return this->pm_irradianceMapSRV; }

	void Draw(VkCommandBuffer commandBuffer, const VkRect2D& renderArea, const Caustics::Constants& constants);

protected:

    Device* pDevice = nullptr;

    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    PostProcCS photonTracer;
    //  ... and denoiser (as building block)

    uint32_t              outWidth = 0, outHeight = 0;
    int                   mipCount_rsm = 0;
    int                   mipCount_gbuf = 0;

    //  photon tracing stuff
    //
    uint32_t              samplingMapScale = 2;
    Texture               samplingMap;
    VkImageView           samplingMapSRV = VK_NULL_HANDLE;

    VkImageView           rsmDepthOpaque1NSRV = VK_NULL_HANDLE;
    VkImageView           gbufDepthOpaque1NSRV = VK_NULL_HANDLE;

    StaticBufferPool      hitpointBuffer;
    VkDescriptorBufferInfo hitPosDescInfo;
    VkDescriptorBufferInfo hitDirDescInfo;

    VkSampler             sampler_default = VK_NULL_HANDLE;
    VkSampler             sampler_depth = VK_NULL_HANDLE;

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

    void generateSamplingPoints(UploadHeap& uploadHeap);
};

