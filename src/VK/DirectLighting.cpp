#include "DirectLighting.h"

#define VERTEX_SHADER_FILENAME "Bypass-vert.glsl"

#define FRAGMENT_SHADER_FILENAME "DirectLighting-frag.glsl"

bool DirectLighting::getAttachmentDesc(std::vector<VkAttachmentDescription>& attachments)
{
    uint32_t numInputAttachments = DLightInput::CameraGBuffer::numImageViews;
    attachments.resize(2);
    int cnt = 0;

    //  output
    //  HDR
    ::AttachBlending(
        VK_FORMAT_R16G16B16A16_SFLOAT, //VK_FORMAT_R16G16B16A16_UNORM,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );
    //  Depth/Stencil
    ::AttachBlending(
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    assert(cnt == 2);
    return true; // because it does consist of depth att.
}

void DirectLighting::OnCreate(
    Device* pDevice, 
    UploadHeap* pUploadHeap, 
    ResourceViewHeaps* pHeaps, 
    DynamicBufferRing* pDynamicBufferRing, 
    StaticBufferPool* pStaticBufferPool,
    VkRenderPass renderPass)
{
    this->pDevice = pDevice;
    this->pResourceViewHeaps = pHeaps;
    this->pDynamicBufferRing = pDynamicBufferRing;
    this->pStaticBufferPool = pStaticBufferPool;

    //  create sampler for sampling GBuffer
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_default);
        assert(res == VK_SUCCESS);
    }

    //  create sampler for sampling RSM (depth)
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.compareEnable = VK_TRUE;
        info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_shadow);
        assert(res == VK_SUCCESS);
    }

    //  create (or derive) render pass
    if (renderPass != VK_NULL_HANDLE)
    {
        this->renderPass = renderPass;
        this->useExternalRenderPass = true;
    }
    else
    {
        this->createRenderPass();
        this->useExternalRenderPass = false;
    }

    //  create descriptor sets and pipeline object for this pass
    DefineList defines;
    this->createDescriptors(&defines);
    this->createPipeline(&defines);
}

void DirectLighting::OnDestroy()
{
    //  destroy pipeline
    vkDestroyPipeline(this->pDevice->GetDevice(), this->pipeline, nullptr);
    vkDestroyPipelineLayout(this->pDevice->GetDevice(), this->pipelineLayout, nullptr);

    //  destroy descriptor sets
    for (int i = 0; i < DLIGHT_NUM_DESCRIPTOR_SETS; i++)
    {
        vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayouts[i], NULL);
        this->pResourceViewHeaps->FreeDescriptor(this->descriptorSets[i]);
    }

    //  destroy render pass
    if (!this->useExternalRenderPass)
    {
        vkDestroyRenderPass(this->pDevice->GetDevice(), this->renderPass, nullptr);
    }
    this->renderPass = VK_NULL_HANDLE;

    //  destroy sampler
    vkDestroySampler(pDevice->GetDevice(), this->sampler_shadow, nullptr);
    vkDestroySampler(pDevice->GetDevice(), this->sampler_default, nullptr);

    this->pDevice = nullptr;
    this->pResourceViewHeaps = nullptr;
    this->pDynamicBufferRing = nullptr;
    this->pStaticBufferPool = nullptr;
}

void DirectLighting::OnCreateWindowSizeDependentResources(
    uint32_t Width, uint32_t Height, GBuffer* pGBuffer)
{
    this->hdrWidth = Width;
    this->hdrHeight = Height;

    //  create frame buffer
    {
        std::vector<VkImageView> attachments = {
            pGBuffer->m_HDRSRV,
            pGBuffer->m_DepthBufferDSV
        };

        this->framebuffer = CreateFrameBuffer(
            this->pDevice->GetDevice(),
            this->renderPass,
            &attachments,
            Width, Height
        );
    }
}

void DirectLighting::OnDestroyWindowSizeDependentResources()
{
    //  destroy frame buffer
    vkDestroyFramebuffer(this->pDevice->GetDevice(), this->framebuffer, nullptr);

    this->hdrWidth = 0;
    this->hdrHeight = 0;
}

void DirectLighting::setCameraGBuffer(DLightInput::CameraGBuffer* pCamSRVs)
{
    //  define input image view descriptions
    uint32_t numInputAttachments = DLightInput::CameraGBuffer::numImageViews;
    std::vector<VkDescriptorImageInfo> desc_image(numInputAttachments);
    desc_image[0].sampler = this->sampler_default;
    desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int i = 1; i < numInputAttachments; i++)
        desc_image[i] = desc_image[0];

    desc_image[0].imageView = pCamSRVs->worldCoord;
    desc_image[1].imageView = pCamSRVs->normal;
    desc_image[2].imageView = pCamSRVs->diffuse;
    desc_image[3].imageView = pCamSRVs->specular;
    desc_image[4].imageView = pCamSRVs->emissive;

    //  update decriptor
    std::vector<VkWriteDescriptorSet> write(numInputAttachments);
    for (unsigned int att = 0; att < write.size(); att++)
    {
        write[att] = {};
        write[att].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[att].pNext = NULL;
        write[att].dstSet = this->descriptorSets[2]; // set 2: G-Buffer
        write[att].descriptorCount = 1;
        write[att].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[att].pImageInfo = &desc_image[att];
        write[att].dstBinding = (uint32_t)att;
        write[att].dstArrayElement = 0;
    }

    vkUpdateDescriptorSets(pDevice->GetDevice(), write.size(), write.data(), 0, NULL);
}

void DirectLighting::setLightGBuffer(DLightInput::LightGBuffer* pLightSRVs)
{
    //  define input image view descriptions
    uint32_t numImages = DLightInput::LightGBuffer::numImageViews;
    std::vector<VkDescriptorImageInfo> desc_image(numImages);
    desc_image[0].sampler = this->sampler_shadow;
    desc_image[0].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    for (int i = 1; i < numImages; i++)
        desc_image[i] = desc_image[0];

    desc_image[0].imageView = pLightSRVs->depthTransparent;
    desc_image[1].imageView = pLightSRVs->stencilTransparent;
    desc_image[2].imageView = pLightSRVs->depthOpaque;

    //  update decriptor
    std::vector<VkWriteDescriptorSet> write(numImages);
    for (unsigned int att = 0; att < write.size(); att++)
    {
        write[att] = {};
        write[att].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[att].pNext = NULL;
        write[att].dstSet = this->descriptorSets[1]; // set 1: RSM
        write[att].descriptorCount = 1;
        write[att].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[att].pImageInfo = &desc_image[att];
        write[att].dstBinding = (uint32_t)att;
        write[att].dstArrayElement = 0;
    }

    vkUpdateDescriptorSets(pDevice->GetDevice(), write.size(), write.data(), 0, NULL);
}

void DirectLighting::Draw(VkCommandBuffer commandBuffer, VkRect2D* renderArea, VkDescriptorBufferInfo* perFrameDesc)
{
    //  begin render pass
    {
        VkRenderPassBeginInfo rp_begin;
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.pNext = NULL;
        rp_begin.renderPass = this->renderPass;
        rp_begin.framebuffer = this->framebuffer;
        rp_begin.renderArea.offset.x = 0;
        rp_begin.renderArea.offset.y = 0;
        rp_begin.renderArea.extent.width = this->hdrWidth;
        rp_begin.renderArea.extent.height = this->hdrHeight;
        rp_begin.pClearValues = NULL;
        rp_begin.clearValueCount = 0;
        vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    }

    SetPerfMarkerBegin(commandBuffer, "D-Light");

    //  set viewport to be rendered
    SetViewportAndScissor(commandBuffer,
        renderArea->offset.x, renderArea->offset.y,
        renderArea->extent.width, renderArea->extent.height);

    //  bind descriptor sets
    uint32_t numUniformOffsets = 1;
    uint32_t uniformOffset = perFrameDesc->offset;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipelineLayout, 0, DLIGHT_NUM_DESCRIPTOR_SETS, this->descriptorSets, numUniformOffsets, &uniformOffset);

    //  bind pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);

    //  draw
    //  ref : https://www.saschawillems.de/blog/2016/08/13/vulkan-tutorial-on-rendering-a-fullscreen-quad-without-buffers/
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    SetPerfMarkerEnd(commandBuffer);

    //  end render pass
    vkCmdEndRenderPass(commandBuffer);
}

void DirectLighting::createRenderPass()
{
    //  get attachment(s) description for this pass
    std::vector<VkAttachmentDescription> attachments;
    DirectLighting::getAttachmentDesc(attachments);

    this->renderPass = CreateRenderPassOptimal(this->pDevice->GetDevice(), attachments.size() - 1, attachments.data(), &attachments.back());
    SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)this->renderPass, "D-Light Renderpass");
}

void DirectLighting::createDescriptors(DefineList* pAttributeDefines)
{
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;

    //  set 0 (general)
    {
        //  define bindings
        layout_bindings.resize(1);
        layout_bindings[0].binding = 0;
        layout_bindings[0].descriptorCount = 1;
        layout_bindings[0].pImmutableSamplers = NULL;
        layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        layout_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        (*pAttributeDefines)["ID_PER_FRAME"] = std::to_string(layout_bindings[0].binding);

        //  create desc. set
        this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
            &layout_bindings,
            &this->descriptorSetLayouts[0],
            &this->descriptorSets[0]);

        //  update perFrame
        this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(per_frame), this->descriptorSets[0]);
    }

    //  set 1 (RSM)
    {
        uint32_t numRSMViews = DLightInput::LightGBuffer::numImageViews;
        (*pAttributeDefines)["ID_shadowMap"] = std::to_string(0);
        this->pResourceViewHeaps->AllocDescriptor(
            numRSMViews,
            nullptr,
            &this->descriptorSetLayouts[1],
            &this->descriptorSets[1]);
    }

    //  set 2 (GBuffer)
    {
        uint32_t numGBufferViews = DLightInput::CameraGBuffer::numImageViews;
        this->pResourceViewHeaps->AllocDescriptor(
            numGBufferViews,
            nullptr,
            &this->descriptorSetLayouts[2],
            &this->descriptorSets[2]);
    }

    //  create the pipeline layout
    {
        VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
        pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pPipelineLayoutCreateInfo.pNext = NULL;
        pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
        pPipelineLayoutCreateInfo.setLayoutCount = DLIGHT_NUM_DESCRIPTOR_SETS;
        pPipelineLayoutCreateInfo.pSetLayouts = this->descriptorSetLayouts;

        VkResult res = vkCreatePipelineLayout(this->pDevice->GetDevice(), &pPipelineLayoutCreateInfo, NULL, &this->pipelineLayout);
        assert(res == VK_SUCCESS);
        SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)this->pipelineLayout, "D-Light PipLayout");
    }
}

void DirectLighting::createPipeline(const DefineList* defines)
{
    //  compile and create shaders
    VkPipelineShaderStageCreateInfo vertexShader = {}, fragmentShader = {};
    {
        VkResult res_compile_shader;
        res_compile_shader = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_VERTEX_BIT, VERTEX_SHADER_FILENAME, "main", "", defines, &vertexShader);
        assert(res_compile_shader == VK_SUCCESS);
        res_compile_shader = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_SHADER_FILENAME, "main", "", defines, &fragmentShader);
        assert(res_compile_shader == VK_SUCCESS);
    }
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = { vertexShader, fragmentShader };

    // vertex input state

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 0;
    vi.pVertexBindingDescriptions = NULL;
    vi.vertexAttributeDescriptionCount = 0;
    vi.pVertexAttributeDescriptions = NULL;

    // input assembly state

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // rasterizer state

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT; // due to EXT_MAINTAINANCE1
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    // Color blend state

    VkPipelineColorBlendAttachmentState att_state = {};
    att_state.colorWriteMask = 0xf;
    att_state.blendEnable = VK_FALSE; // VK_TRUE;
    att_state.alphaBlendOp = VK_BLEND_OP_ADD;
    att_state.colorBlendOp = VK_BLEND_OP_ADD;
    att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    cb.attachmentCount = 1;
    cb.pAttachments = &att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    // dynamic state

    std::vector<VkDynamicState> dynamicStateEnables = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables.data();
    dynamicState.dynamicStateCount = (uint32_t)dynamicStateEnables.size();

    // view port state

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    vp.pScissors = NULL;
    vp.pViewports = NULL;

    // depth stencil state

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_ZERO; // reset the stencil value
    ds.back.compareOp = VK_COMPARE_OP_EQUAL;
    ds.back.compareMask = 0xff;
    ds.back.reference = 1;
    ds.back.writeMask = 0xff;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.stencilTestEnable = VK_TRUE;
    ds.front = ds.back;

    // multi sample state

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    //  create pipeline
    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = NULL;
    pipeline.layout = this->pipelineLayout;
    pipeline.basePipelineHandle = VK_NULL_HANDLE;
    pipeline.basePipelineIndex = 0;
    pipeline.flags = 0;
    pipeline.pVertexInputState = &vi;
    pipeline.pInputAssemblyState = &ia;
    pipeline.pRasterizationState = &rs;
    pipeline.pColorBlendState = &cb;
    pipeline.pTessellationState = NULL;
    pipeline.pMultisampleState = &ms;
    pipeline.pDynamicState = &dynamicState;
    pipeline.pViewportState = &vp;
    pipeline.pDepthStencilState = &ds;
    pipeline.pStages = shaderStages.data();
    pipeline.stageCount = (uint32_t)shaderStages.size();
    pipeline.renderPass = this->renderPass;
    pipeline.subpass = 0;

    VkResult res = vkCreateGraphicsPipelines(this->pDevice->GetDevice(), this->pDevice->GetPipelineCache(), 1, &pipeline, NULL, &this->pipeline);
    assert(res == VK_SUCCESS);
    SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)this->pipeline, "D-Light Pipeline");
}
