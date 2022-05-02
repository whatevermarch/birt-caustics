#pragma once

class SVGF
{
public:

    struct Constants
    {
        float alphaColor;
        float alphaMoments;
        float nearPlane;
        float farPlane;

        float sigmaDepth;
        float sigmaNormal;
        float sigmaLuminance;
        float padding;
    };

    void OnCreate(
        Device* pDevice,
        ResourceViewHeaps* pResourceViewHeaps,
        DynamicBufferRing* pDynamicBufferRing);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(
        uint32_t Width, uint32_t Height, 
        Texture* pTarget, VkImageView targetSRV, 
        VkImageView depthSRV, GBuffer* pGBuffer);
    void OnDestroyWindowSizeDependentResources();

    void Draw(VkCommandBuffer commandBuffer, const SVGF::Constants& constants);

private:

    Device* pDevice;
    ResourceViewHeaps* pResourceViewHeaps;
    DynamicBufferRing* pDynamicBufferRing;

    uint32_t              outWidth = 0, outHeight = 0;

    Texture*              pInputHDR = nullptr;
    VkImageView           inputHDRSRV = VK_NULL_HANDLE;
    GBuffer*              pInputGBuffer = nullptr;

    Texture               cache_HDR, // r16g16b16a16f
                          cache_Normal, // r16g16b16a16f
                          cache_DepthMoment, // d16 + f16 (can d16 cuz it's linear depth) + r16g16
                          cache_History; //r8u
    VkImageView           cache_HDRSRV = VK_NULL_HANDLE,
                          cache_NormalSRV = VK_NULL_HANDLE,
                          cache_DepthMomentSRV = VK_NULL_HANDLE,
                          cache_HistorySRV = VK_NULL_HANDLE;

    Texture               imd_HDR, // r16g16b16a16
                          imd_DepthMoment, // d16 + f16 (can d16 cuz it's linear depth) + r16g16
                          imd_History; //r8u
    VkImageView           imd_HDRSRV = VK_NULL_HANDLE,
                          imd_DepthMomentSRV = VK_NULL_HANDLE,
                          imd_HistorySRV = VK_NULL_HANDLE;

    VkSampler             sampler_default;

    VkDescriptorSet       ta_descriptorSet;
    VkDescriptorSetLayout ta_descriptorSetLayout;
    VkRenderPass          ta_renderPass;
    VkFramebuffer         ta_framebuffer;
    PostProcPS            tmpAccum;

    void createTADescriptors(DefineList& defines);
    void barrier_TA(VkCommandBuffer cmdBuf);

    VkDescriptorSet       ve_descriptorSet;
    VkDescriptorSetLayout ve_descriptorSetLayout;
    PostProcCS            varEst;

    void createVEDescriptors(DefineList& defines);
    void barrier_VE(VkCommandBuffer cmdBuf);

    VkDescriptorSet       at_descriptorSet;
    VkDescriptorSetLayout at_descriptorSetLayout;
    PostProcCS            aTrous;

    void createATDescriptors(DefineList& defines);
    void setATInOutHDR(VkCommandBuffer cmdBuf, uint32_t atrousIter);
    void barrier_AT(VkCommandBuffer cmdBuf);
    void barrier_AT_PerIter(VkCommandBuffer cmdBuf, uint32_t atrousIter);

    void barrier_Out(VkCommandBuffer cmdBuf);
};