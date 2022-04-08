#pragma once

class Ocean
{
public:
    struct Constants
    {
        XMMATRIX currWorld;
        XMMATRIX prevWorld;
        XMMATRIX currViewProj;
        XMMATRIX prevViewProj;

        Light rsmLight;

        static XMMATRIX calculateWorldMatrix(const XMFLOAT3& center, const XMFLOAT3& scale)
        {
            assert(scale.x != 0 && scale.y != 0 && scale.z != 0);
            XMMATRIX world{};

            XMFLOAT4 rowElems{};
            rowElems = { scale.x, 0, 0, 0 };
            world.r[0] = XMLoadFloat4(&rowElems);
            rowElems = { 0, scale.y, 0, 0 };
            world.r[1] = XMLoadFloat4(&rowElems);
            rowElems = { 0, 0, scale.z, 0 };
            world.r[2] = XMLoadFloat4(&rowElems);
            rowElems = { center.x, center.y, center.z, 1 };
            world.r[3] = XMLoadFloat4(&rowElems);

            return world;
        }
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pResourceViewHeaps, 
        DynamicBufferRing* pDynamicBufferRing,
        const char* normalMapsFilepath,
        GBufferRenderPass* rsmRenderPass,
        GBufferRenderPass* gbufRenderPass,
        VkSampleCountFlagBits sampleCount);
    void OnDestroy();
    void Draw(VkCommandBuffer cmdBuf, const Ocean::Constants& constants, uint32_t iter, int32_t rsmLightIndex = -1);

protected:
    Device* m_pDevice{ nullptr };
    ResourceViewHeaps* m_pResourceViewHeaps{ nullptr };
    DynamicBufferRing* m_pDynamicBufferRing{ nullptr };

    Texture m_normalMapArray;
    VkImageView m_normalMapArraySRV{ VK_NULL_HANDLE };
    VkSampler m_samplerDefault{ VK_NULL_HANDLE };

    VkDescriptorSet       m_descriptorSet{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_descriptorSetLayout{ VK_NULL_HANDLE };

    enum class Pass
    {
        RSM,
        GBuffer
    };
    struct PushConstants
    {
        uint32_t iter;
        int32_t rsmLightIndex;
    };
    VkPipeline m_pipelines[2]{ VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout{ VK_NULL_HANDLE };

    void createDescriptors(DefineList &defines);
    void createPipeline(Ocean::Pass pass, VkRenderPass renderPass, VkSampleCountFlagBits sampleCount, const DefineList& defines);
};

