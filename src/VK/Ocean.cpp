#include "Ocean.h"

#define VERTEX_SHADER_FILENAME "Ocean-vert.glsl"
#define FRAGMENT_SHADER_FILENAME "Ocean-frag.glsl"

void Ocean::OnCreate(
	Device* pDevice,
	UploadHeap* pUploadHeap,
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing, 
	const char* normalMapsFilepath,
    GBufferRenderPass* rsmRenderPass,
    GBufferRenderPass* gbufRenderPass,
	VkSampleCountFlagBits sampleCount)
{
	m_pDevice = pDevice;
	m_pResourceViewHeaps = pResourceViewHeaps;
	m_pDynamicBufferRing = pDynamicBufferRing;

	// upload textures
    constexpr uint32_t fileStart = 1;
    constexpr uint32_t fileEnd = 20;
    bool res = m_normalMapArray.InitFromSeries(
        pDevice,
        pUploadHeap,
        normalMapsFilepath,
        fileStart, fileEnd - fileStart + 1,
        false
    );
    assert(res);

	pUploadHeap->FlushAndFinish();

	m_normalMapArray.CreateSRV(&m_normalMapArraySRV);
    m_samplerDefault = [&]() -> VkSampler {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = info.magFilter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;

        VkSampler sampler;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &sampler);
        assert(res == VK_SUCCESS);

        return sampler;
    }();

	// create descriptors
    DefineList defines, rsmDefines, gbufDefines;
	this->createDescriptors(defines);

    // setup descriptors
    pDynamicBufferRing->SetDescriptorSet(0, sizeof(Ocean::Constants), m_descriptorSet);
    SetDescriptorSet(pDevice->GetDevice(), 1, m_normalMapArraySRV, &m_samplerDefault, m_descriptorSet);

	// create pipeline
    if (rsmRenderPass != nullptr)
    {
        rsmDefines = defines;
        rsmRenderPass->GetCompilerDefines(rsmDefines);
        this->createPipeline(Pass::RSM, rsmRenderPass->GetRenderPass(), sampleCount, rsmDefines);
    }

    gbufDefines = defines;
    gbufRenderPass->GetCompilerDefines(gbufDefines);
    this->createPipeline(Pass::GBuffer, gbufRenderPass->GetRenderPass(), sampleCount, gbufDefines);
}

void Ocean::OnDestroy()
{
    for (uint32_t i = 0; i < 2; i++)
    {
        if(m_pipelines[i] != VK_NULL_HANDLE)
            vkDestroyPipeline(m_pDevice->GetDevice(), m_pipelines[i], nullptr);
    }
    vkDestroyPipelineLayout(m_pDevice->GetDevice(), m_pipelineLayout, nullptr);

	vkDestroyDescriptorSetLayout(m_pDevice->GetDevice(), m_descriptorSetLayout, nullptr);

	m_pResourceViewHeaps->FreeDescriptor(m_descriptorSet);

    vkDestroySampler(m_pDevice->GetDevice(), m_samplerDefault, nullptr);
    vkDestroyImageView(m_pDevice->GetDevice(), m_normalMapArraySRV, nullptr);

	m_normalMapArray.OnDestroy();

	m_pDevice = nullptr;
	m_pResourceViewHeaps = nullptr;
	m_pDynamicBufferRing = nullptr;
}

void Ocean::Draw(VkCommandBuffer cmdBuf, const Ocean::Constants& constants, uint32_t iter, int32_t rsmLightIndex)
{
    SetPerfMarkerBegin(cmdBuf, "Ocean");

    Ocean::Constants* cbPerDraw;
    VkDescriptorBufferInfo constantBuffer;
    m_pDynamicBufferRing->AllocConstantBuffer(sizeof(Ocean::Constants), (void**)&cbPerDraw, &constantBuffer);
    *cbPerDraw = constants;

    // Bind Descriptor sets
    const uint32_t numUniformOffsets = 1;
    const uint32_t uniformOffset = (uint32_t)constantBuffer.offset;
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, numUniformOffsets, &uniformOffset);

    // Bind Pipeline
    Ocean::Pass pass = rsmLightIndex >= 0 && m_pipelines[(size_t)Pass::RSM] != VK_NULL_HANDLE ?
        Pass::RSM : Pass::GBuffer;
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[(size_t)pass]);

    // Push constants
    Ocean::PushConstants pushConstants = { iter, rsmLightIndex };
    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pushConstants);

    // Draw
    vkCmdDraw(cmdBuf, 6 /* ToDo: 4 */, 1, 0, 0);

    SetPerfMarkerEnd(cmdBuf);
}

void Ocean::createDescriptors(DefineList& defines)
{
	const uint32_t bindingCount = 2;
	std::vector<VkDescriptorSetLayoutBinding> layout_bindings(bindingCount);
	uint32_t bindingIdx = 0;

	layout_bindings[0].binding = bindingIdx;
	layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layout_bindings[0].descriptorCount = 1;
    layout_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	layout_bindings[0].pImmutableSamplers = NULL;
	defines["ID_Params"] = std::to_string(bindingIdx++);

	layout_bindings[1].binding = bindingIdx;
	layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layout_bindings[1].descriptorCount = 1;
	layout_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layout_bindings[1].pImmutableSamplers = NULL;
	defines["ID_NormalMapArray"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	m_pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layout_bindings, &m_descriptorSetLayout, &m_descriptorSet);
}

void Ocean::createPipeline(Ocean::Pass pass, VkRenderPass renderPass, VkSampleCountFlagBits sampleCount, const DefineList& defines)
{
	VkResult res;
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

	VkPipelineShaderStageCreateInfo vertexShader;
    res = VKCompileFromFile(m_pDevice->GetDevice(), VK_SHADER_STAGE_VERTEX_BIT, VERTEX_SHADER_FILENAME, "main", "", &defines, &vertexShader);
	assert(res == VK_SUCCESS);
    shaderStages.push_back(vertexShader);

    // ToDo: tesselation shader
    // ...

	VkPipelineShaderStageCreateInfo fragmentShader;
	res = VKCompileFromFile(m_pDevice->GetDevice(), VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_SHADER_FILENAME, "main", "", &defines, &fragmentShader);
	assert(res == VK_SUCCESS);
    shaderStages.push_back(fragmentShader);

    // create pipeline layout
    // push const 0: iteration to switch normal maps 
    VkPushConstantRange pushConstRange = {};
    pushConstRange.offset = 0;
    pushConstRange.size = sizeof(Ocean::PushConstants);
    pushConstRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.pNext = NULL;
    pPipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pPipelineLayoutCreateInfo.pPushConstantRanges = &pushConstRange;
    pPipelineLayoutCreateInfo.setLayoutCount = 1;
    pPipelineLayoutCreateInfo.pSetLayouts = &m_descriptorSetLayout;

    res = vkCreatePipelineLayout(m_pDevice->GetDevice(), &pPipelineLayoutCreateInfo, NULL, &m_pipelineLayout);
    assert(res == VK_SUCCESS);
    SetResourceName(m_pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)m_pipelineLayout, "Ocean PL");

    /////////////////////////////////////////////
    // vertex input state

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 0;
    vi.pVertexBindingDescriptions = NULL;
    vi.vertexAttributeDescriptionCount = 0;
    vi.pVertexAttributeDescriptions = NULL;

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // ToDo: VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

    // rasterizer state

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    std::vector<VkPipelineColorBlendAttachmentState> att_states;
    if (defines.Has("HAS_FORWARD_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = (defines.Has("DEF_alphaMode_BLEND"));
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_SPECULAR_ROUGHNESS_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_DIFFUSE_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_NORMALS_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_WORLD_COORD_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_EMISSIVE_FLUX_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }
    if (defines.Has("HAS_MOTION_VECTORS_RT"))
    {
        VkPipelineColorBlendAttachmentState att_state = {};
        att_state.colorWriteMask = 0xf;
        att_state.blendEnable = VK_FALSE;
        att_state.alphaBlendOp = VK_BLEND_OP_ADD;
        att_state.colorBlendOp = VK_BLEND_OP_ADD;
        att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att_states.push_back(att_state);
    }

    // Color blend state

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    cb.attachmentCount = static_cast<uint32_t>(att_states.size());
    cb.pAttachments = att_states.data();
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    std::vector<VkDynamicState> dynamicStateEnables = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE // adjust ref val per obj type (opaque/trans)
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
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_TRUE;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_REPLACE;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0xff;
    ds.back.reference = 1;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0xff;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.front = ds.back;

    // multi sample state

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = sampleCount;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    // create pipeline 

    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = NULL;
    pipeline.layout = m_pipelineLayout;
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
    pipeline.renderPass = renderPass;
    pipeline.subpass = 0;

    auto createPipelineObj = [&](uint32_t pipelineIdx, const char* name) {
        res = vkCreateGraphicsPipelines(m_pDevice->GetDevice(), m_pDevice->GetPipelineCache(), 1, &pipeline, NULL, &m_pipelines[pipelineIdx]);
        assert(res == VK_SUCCESS);
        SetResourceName(m_pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)m_pipelines[pipelineIdx], name);
    };

    switch (pass)
    {
    case Pass::RSM:
        createPipelineObj((size_t)pass, "Ocean Pipeline (RSM)");
        break;
    case Pass::GBuffer:
        createPipelineObj((size_t)pass, "Ocean Pipeline (G-Buffer)");
        break;
    }
}
