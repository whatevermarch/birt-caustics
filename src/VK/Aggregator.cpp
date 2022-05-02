#include "Aggregator.h"

#define WG_SIZE_XY 32

void Aggregator::OnCreate(
	Device* pDevice, 
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing,
    uint32_t inputFxCount)
{
    this->pDevice = pDevice;
    this->pDynamicBufferRing = pDynamicBufferRing;
    this->pResourceViewHeaps = pResourceViewHeaps;

    assert(inputFxCount <= 3);
    this->numInputFx = inputFxCount;

    //  create default sampler for i-light input
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_default);
        assert(res == VK_SUCCESS);
    }

    //  create pipeline
    DefineList defines;
    this->createDescriptor(defines);
    
    this->aggregator.OnCreate(
        this->pDevice, "Aggregator.glsl", "main", "", 
        this->descriptorSetLayout, 0, 0, 0, &defines);
}

void Aggregator::OnDestroy()
{
    vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);

    this->aggregator.OnDestroy();

    this->pResourceViewHeaps->FreeDescriptor(this->descriptorSet);
    vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayout, NULL);

    pDevice = nullptr;
    pResourceViewHeaps = nullptr;
    pDynamicBufferRing = nullptr;
}

void Aggregator::UpdateInputs(uint32_t targetWidth, uint32_t targetHeight,
        VkImageView targetSRV, VkImageView inputFxSRVs[3])
{
    this->outWidth = targetWidth;
    this->outHeight = targetHeight;

    VkDescriptorImageInfo imgInfos[4];
    VkWriteDescriptorSet writes[4];
    const uint32_t numInputImages = 1 + this->numInputFx;

    //  of target input
    imgInfos[0].sampler = VK_NULL_HANDLE;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfos[0].imageView = targetSRV;

    writes[0] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = NULL;
    writes[0].dstSet = this->descriptorSet;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imgInfos[0];
    writes[0].dstBinding = 1;
    writes[0].dstArrayElement = 0;

    //  of input fx
    for(uint32_t i = 1; i <= this->numInputFx; i++)
    {
        imgInfos[i].sampler = this->sampler_default;
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[i].imageView = inputFxSRVs[i - 1];

        writes[i] = writes[0];
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
        writes[i].dstBinding = i + 1;
    }

    vkUpdateDescriptorSets(this->pDevice->GetDevice(), numInputImages, writes, 0, NULL);
}

void Aggregator::Draw(VkCommandBuffer cmdbuf, const float weights[4])
{
    ::SetPerfMarkerBegin(cmdbuf, "Aggregator");

    //  update constants
    VkDescriptorBufferInfo descInfo_constants;
    {
        Aggregator::Constants* pAllocData;
        this->pDynamicBufferRing->AllocConstantBuffer(sizeof(Aggregator::Constants), (void**)&pAllocData, &descInfo_constants);
        pAllocData->weights = XMLoadFloat4((const XMFLOAT4*)weights);
        pAllocData->imgWidth = this->outWidth;
        pAllocData->imgHeight = this->outHeight;
    }

    //  dispatch
    uint32_t numWG_x = (this->outWidth + WG_SIZE_XY - 1) / WG_SIZE_XY;
    uint32_t numWG_y = (this->outHeight + WG_SIZE_XY - 1) / WG_SIZE_XY;
    this->aggregator.Draw(cmdbuf, &descInfo_constants, this->descriptorSet, numWG_x, numWG_y, 1);

    ::SetPerfMarkerEnd(cmdbuf);
}

void Aggregator::createDescriptor(DefineList& defines)
{
    //  define bindings
    const uint32_t numInputs = 2 + this->numInputFx;
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(numInputs);
    uint32_t inputIdx = 0;

    layoutBindings[inputIdx].binding = inputIdx;
    layoutBindings[inputIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    layoutBindings[inputIdx].descriptorCount = 1;
    layoutBindings[inputIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layoutBindings[inputIdx].pImmutableSamplers = NULL;
    defines["ID_Params"] = std::to_string(inputIdx++);

    layoutBindings[inputIdx].binding = inputIdx;
    layoutBindings[inputIdx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[inputIdx].descriptorCount = 1;
    layoutBindings[inputIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layoutBindings[inputIdx].pImmutableSamplers = NULL;
    defines["ID_Target"] = std::to_string(inputIdx++);

    for (uint32_t i = 0; i < this->numInputFx; i++)
    {
        layoutBindings[inputIdx].binding = inputIdx;
        layoutBindings[inputIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        layoutBindings[inputIdx].descriptorCount = 1;
        layoutBindings[inputIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        layoutBindings[inputIdx].pImmutableSamplers = NULL;
        defines[std::string("ID_FX") + std::to_string(i)] = std::to_string(inputIdx++);
    }

    //  allocate descriptor
    assert(inputIdx == numInputs);
    this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
        &layoutBindings,
        &this->descriptorSetLayout,
        &this->descriptorSet);

    //  immediately update binding 0 (dynamic buffer)
    this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(Aggregator::Constants), this->descriptorSet);
}
