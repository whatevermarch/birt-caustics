#pragma once

class Aggregator
{
public:
    struct Constants
    {
        XMVECTOR weights;

        uint32_t imgWidth, imgHeight;
        float paddings[2];
    };

    void OnCreate(
        Device* pDevice, 
        ResourceViewHeaps* pResourceViewHeaps, 
        DynamicBufferRing* pDynamicBufferRing,
        uint32_t inputFxCount);
    void OnDestroy();

    void UpdateInputs(uint32_t targetWidth, uint32_t targetHeight,
        VkImageView targetSRV, VkImageView inputFxSRVs[3]);

    void Draw(VkCommandBuffer cmdbuf, const float weights[4]);

private:
    Device* pDevice = nullptr;
    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    PostProcCS aggregator;

    uint32_t outWidth = 0, outHeight = 0;
    uint32_t numInputFx = 0;
    VkSampler sampler_default;

    VkDescriptorSet       descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    void createDescriptor(DefineList& defines);
};
