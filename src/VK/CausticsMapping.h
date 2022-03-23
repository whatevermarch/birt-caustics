#pragma once

class CausticsMapping
{
public:

    struct Constants
    {
        //  per-object
        XMMATRIX world;

        //  per-frame
        XMMATRIX lightView;
        float lightInvTanHalfFovH;
        float lightInvTanHalfFovV;
        float lightFRange;
        float lightNearZ;
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pResourceViewHeaps,
        DynamicBufferRing* pDynamicBufferRing,
        uint32_t mapWidth, uint32_t mapHeight,
        GBuffer* pRSM, VkImageView rsmDepthOpaqueSRV,
        VkRenderPass reprojectionRenderPass);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(
        uint32_t Width, uint32_t Height,
        GBuffer* pGBuffer, VkImageView gbufDepthOpaqueSRV,
        VkFramebuffer reprojectionFramebuffer);
    void OnDestroyWindowSizeDependentResources();

    void registerScene(GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers);
    void deregisterScene();

    Texture* GetTexture() { return &this->causticsMap; }
    const Texture* GetTexture() const { return &this->causticsMap; }
    VkImageView GetTextureView() { return this->causticsMapSRV; }

    void Draw(VkCommandBuffer commandBuffer, const VkRect2D& renderArea, const CausticsMapping::Constants& constants);
    
protected:

    Device* pDevice = nullptr;

    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    GBuffer* pRSM = nullptr, *pGBuffer = nullptr;

    uint32_t              screenWidth = 0, screenHeight = 0;

    //  Workaround : only one transparent object -> water surface
    Geometry              geometry;
    XMMATRIX              geomWorld{ XMMatrixIdentity() };
    std::vector<VkVertexInputAttributeDescription> inputLayout;
    DefineList            baseDefines;

    uint32_t              causticsMapWidth = 0, causticsMapHeight = 0;
    Texture               causticsMap;
    VkImageView           causticsMapSRV = VK_NULL_HANDLE;

    VkSampler             sampler_default, sampler_depth;

    VkDescriptorSet       descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    VkPipeline            pipeline;
    VkPipelineLayout      pipelineLayout;

    VkRenderPass          renderPass;

    VkFramebuffer         framebuffer;

    void createDescriptors(DefineList* pDefines);
    void createPipeline(const DefineList& defines);

    //  for reprojecting caustics to camera view
    PostProcPS            reproj;

    VkDescriptorSetLayout reproj_descriptorSetLayout;
    VkDescriptorSet       reproj_descriptorSet;

    VkRenderPass          reproj_renderPass = VK_NULL_HANDLE;
    VkFramebuffer         reproj_framebuffer = VK_NULL_HANDLE;

    void createReprojDescriptors(DefineList* pDefines);
};

