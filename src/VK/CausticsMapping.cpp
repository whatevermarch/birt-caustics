#include "CausticsMapping.h"
#include "../../Cauldron/src/VK/GLTF/glTFHelpers.h"

#define VERTEX_SHADER_FILENAME "CausticsMapping-vert.glsl"

#define FRAGMENT_SHADER_FILENAME "CausticsMapping-frag.glsl"

void CausticsMapping::OnCreate(
	Device* pDevice, 
	UploadHeap* pUploadHeap, 
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing, 
	uint32_t mapWidth, uint32_t mapHeight,
	GBuffer* pRSM, VkImageView rsmDepthOpaqueSRV,
	VkRenderPass reprojectionRenderPass)
{
	this->pDevice = pDevice;
	this->pResourceViewHeaps = pResourceViewHeaps;
	this->pDynamicBufferRing = pDynamicBufferRing;

	this->pRSM = pRSM;

	this->causticsMapWidth = mapWidth;
	this->causticsMapHeight = mapHeight;

	//	create caustics map texture
	this->causticsMap.InitRenderTarget(
		this->pDevice,
		this->causticsMapWidth, this->causticsMapHeight,
		VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
		false,
		"Caustics Map"
	);
	this->causticsMap.CreateSRV(&this->causticsMapSRV);

	//  create default/depth sampler
	this->sampler_default = CreateSampler(pDevice->GetDevice(), true);
	this->sampler_depth = CreateSampler(pDevice->GetDevice(), false);

	//	setup shader inputs
	// 
	this->baseDefines.clear();
	this->createDescriptors(&this->baseDefines);
	this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(CausticsMapping::Constants), this->descriptorSet);
	SetDescriptorSetForDepth(this->pDevice->GetDevice(), 1, rsmDepthOpaqueSRV, &this->sampler_depth, this->descriptorSet);

	//	create render pass (for caustics map generation)
	VkAttachmentDescription att_desc[1];
	AttachClearBeforeUse(
		VK_FORMAT_R16G16B16A16_SFLOAT,
		(VkSampleCountFlagBits)1,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		&att_desc[0]);
	this->renderPass = CreateRenderPassOptimal(this->pDevice->GetDevice(), 1, att_desc, nullptr);

	//	create framebuffer
	std::vector<VkImageView> attachments = {
			this->causticsMapSRV
	};
	this->framebuffer = CreateFrameBuffer(
		this->pDevice->GetDevice(),
		this->renderPass,
		&attachments,
		this->causticsMapWidth, this->causticsMapHeight
	);

	//	setup reprojection pipeline
	{
		this->reproj_renderPass = reprojectionRenderPass;

		DefineList reproj_defines;
		this->createReprojDescriptors(&reproj_defines);
		this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(CausticsMapping::Constants), this->reproj_descriptorSet);
		SetDescriptorSetForDepth(this->pDevice->GetDevice(), 2, rsmDepthOpaqueSRV, &this->sampler_depth, this->reproj_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 3, this->causticsMapSRV, &this->sampler_default, this->reproj_descriptorSet);

		this->reproj.OnCreate(
			pDevice, this->reproj_renderPass,
			"CausticsMapReproj.glsl", "main", "", nullptr,
			pDynamicBufferRing, this->reproj_descriptorSetLayout,
			0, VK_SAMPLE_COUNT_1_BIT, &reproj_defines
		);
	}
}

void CausticsMapping::OnDestroy()
{
	this->reproj.OnDestroy();
	this->pResourceViewHeaps->FreeDescriptor(this->reproj_descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->reproj_descriptorSetLayout, nullptr);

	this->reproj_renderPass = VK_NULL_HANDLE;

	vkDestroyFramebuffer(this->pDevice->GetDevice(), this->framebuffer, nullptr);

	vkDestroyRenderPass(this->pDevice->GetDevice(), this->renderPass, nullptr);

	this->pResourceViewHeaps->FreeDescriptor(this->descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayout, nullptr);

	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);
	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_depth, nullptr);

	this->causticsMap.OnDestroy();
	vkDestroyImageView(this->pDevice->GetDevice(), this->causticsMapSRV, nullptr);

	this->pDevice = nullptr;
	this->pResourceViewHeaps = nullptr;
	this->pDynamicBufferRing = nullptr;
}

void CausticsMapping::OnCreateWindowSizeDependentResources(
	uint32_t Width, uint32_t Height,
	GBuffer* pGBuffer, VkImageView gbufDepthOpaqueSRV,
	VkFramebuffer reprojectionFramebuffer)
{
	this->pGBuffer = pGBuffer;

	this->screenWidth = Width;
	this->screenHeight = Height;

	this->reproj_framebuffer = reprojectionFramebuffer;

	//	setup shader inputs
	SetDescriptorSet(this->pDevice->GetDevice(), 1, pGBuffer->m_WorldCoordSRV, &this->sampler_default, this->reproj_descriptorSet);
}

void CausticsMapping::OnDestroyWindowSizeDependentResources()
{
	this->pGBuffer = nullptr;

	this->reproj_framebuffer = VK_NULL_HANDLE;
}

void CausticsMapping::registerScene(GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers)
{
	DefineList defines = this->baseDefines;
	int waterMaterialIdx{ -1 };
	const json& j3 = pGLTFTexturesAndBuffers->m_pGLTFCommon->j3;

	//	we need only water material, 
	//	so if we find water's material, save its index!
	if (j3.find("materials") != j3.end())
	{
		const json& materials = j3["materials"];
		for (uint32_t i = 0; i < materials.size(); i++)
		{
			const json& material = materials[i];

			bool bBlending = GetElementString(material, "alphaMode", "OPAQUE") == "BLEND";
			if (bBlending)
			{
				waterMaterialIdx = i;
				break;
			}
		}
	}
	assert(waterMaterialIdx != -1);

	//	now find the water's mesh
	if (j3.find("meshes") != j3.end())
	{
		const json& meshes = j3["meshes"];

		std::vector<tfNode>* pNodes = &pGLTFTexturesAndBuffers->m_pGLTFCommon->m_nodes;
		for (uint32_t i = 0; i < pNodes->size(); i++)
		{
			if (waterMaterialIdx == -1)
				break;

			tfNode* pNode = &pNodes->at(i);
			if ((pNode == NULL) || (pNode->meshIndex < 0))
				continue;

			const json& primitives = meshes[pNode->meshIndex]["primitives"];
			for (uint32_t p = 0; p < primitives.size(); p++)
			{
				const json& primitive = primitives[p];

				auto mat = primitive.find("material");
				if (mat != primitive.end())
				{
					//	choose only water surface (transparent)
					if (mat.value() == waterMaterialIdx)
					{
						// make a list of all the attribute names our pass requires, in the case of a depth pass we only need the position and a few other things. 
						//
						std::vector<std::string> requiredAttributes;
						for (auto const& it : primitive["attributes"].items())
						{
							const std::string semanticName = it.key();
							if (
								(semanticName == "POSITION") ||
								(semanticName == "NORMAL")
								)
							{
								requiredAttributes.push_back(semanticName);
							}
						}
						assert(requiredAttributes.size() == 2);

						//	swap attributes order if needed
						//	Remember that in OnCreate(), we define POSITION as 0, and NORMAL as 1.
						if (requiredAttributes.front().compare("POSITION") != 0)
						{
							std::string tmp = requiredAttributes.front();
							requiredAttributes.front() = requiredAttributes.back();
							requiredAttributes.back() = tmp;
						}

						pGLTFTexturesAndBuffers->CreateGeometry(primitive, requiredAttributes, this->inputLayout, defines, &this->geometry);
						this->geomWorld = pGLTFTexturesAndBuffers->m_pGLTFCommon->m_worldSpaceMats.data()[i].GetCurrent();

						waterMaterialIdx = -1;
						break;
					}
				}
			}
		}
	}

	//	create pipeline for point renderer
	this->createPipeline(defines);
}

void CausticsMapping::deregisterScene()
{
	vkDestroyPipeline(this->pDevice->GetDevice(), this->pipeline, nullptr);
	vkDestroyPipelineLayout(this->pDevice->GetDevice(), this->pipelineLayout, nullptr);
}

void CausticsMapping::Draw(
	VkCommandBuffer commandBuffer, const VkRect2D& renderArea, 
	const CausticsMapping::Constants& constants)
{
	SetPerfMarkerBegin(commandBuffer, "Caustics Mapping");

	//  update constants
	VkDescriptorBufferInfo descInfo_constants;
	{
		CausticsMapping::Constants* pAllocData;
		this->pDynamicBufferRing->AllocConstantBuffer(sizeof(CausticsMapping::Constants), (void**)&pAllocData, &descInfo_constants);
		*pAllocData = constants;

		//	workaround : fill world mattrix manually from inside this class
		pAllocData->world = this->geomWorld;
	}

	//	start render pass
	VkClearValue cv{ 0.f, 0.f, 0.f, 0.f };

	//	generating caustics map first
	{
		VkRenderPassBeginInfo rp_begin{};
		rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rp_begin.pNext = NULL;
		rp_begin.renderPass = this->renderPass;
		rp_begin.framebuffer = this->framebuffer;
		rp_begin.renderArea.offset.x = 0;
		rp_begin.renderArea.offset.y = 0;
		rp_begin.renderArea.extent.width = this->causticsMapWidth;
		rp_begin.renderArea.extent.height = this->causticsMapHeight;
		rp_begin.clearValueCount = 1;
		rp_begin.pClearValues = &cv;
		vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		SetViewportAndScissor(commandBuffer,
			0, 0, this->causticsMapWidth, this->causticsMapHeight);

		if (this->inputLayout.size() != 0)
		{
			// Bind vertices 
			//
			for (uint32_t i = 0; i < this->geometry.m_VBV.size(); i++)
			{
				vkCmdBindVertexBuffers(commandBuffer, i, 1, &this->geometry.m_VBV[i].buffer, &this->geometry.m_VBV[i].offset);
			}

			// Bind Descriptor sets
			//
			uint32_t uniformOffset = descInfo_constants.offset;
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipelineLayout, 0, 1, &this->descriptorSet, 1, &uniformOffset);

			// Bind Pipeline
			//
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);

			// Draw
			// workaround : there is totally 2500(x1)/9801(x2)/38809(x4) vertices for water surface. Geometry class doesn't store that value.
			//
			vkCmdDraw(commandBuffer, 38809 /*this->geometry.m_NumIndices*/, 1, 0, 0);
		}

		//	end render pass
		vkCmdEndRenderPass(commandBuffer);
	}

	//	then reprojecting
	//
	{
		VkRenderPassBeginInfo rp_begin{};
		rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rp_begin.pNext = NULL;
		rp_begin.renderPass = this->reproj_renderPass;
		rp_begin.framebuffer = this->reproj_framebuffer;
		rp_begin.renderArea.offset.x = 0;
		rp_begin.renderArea.offset.y = 0;
		rp_begin.renderArea.extent.width = this->screenWidth;
		rp_begin.renderArea.extent.height = this->screenHeight;
		rp_begin.clearValueCount = 1;
		rp_begin.pClearValues = &cv;
		vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		SetViewportAndScissor(commandBuffer,
			renderArea.offset.x, renderArea.offset.y,
			renderArea.extent.width, renderArea.extent.height);

		if (this->inputLayout.size() != 0)
		{
			this->reproj.Draw(commandBuffer, &descInfo_constants, this->reproj_descriptorSet);
		}

		vkCmdEndRenderPass(commandBuffer);
	}

	SetPerfMarkerEnd(commandBuffer);
}

void CausticsMapping::createDescriptors(DefineList* pDefines)
{
	const uint32_t bindingCount = 2;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;

	//	input
	//
	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_Params"] = std::to_string(bindingIdx++);
	//	1. RSM depth
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_Depth"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->descriptorSetLayout,
		&this->descriptorSet);
}

void CausticsMapping::createPipeline(const DefineList& defines)
{
	//	create pipeline layout
	//
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCreateInfo.pNext = NULL;
	pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
	pipelineLayoutCreateInfo.pPushConstantRanges = NULL;
	pipelineLayoutCreateInfo.setLayoutCount = 1;
	pipelineLayoutCreateInfo.pSetLayouts = &this->descriptorSetLayout;

	VkResult res = vkCreatePipelineLayout(this->pDevice->GetDevice(), &pipelineLayoutCreateInfo, NULL, &this->pipelineLayout);
	assert(res == VK_SUCCESS);
	SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)this->pipelineLayout, "Caustics Mapping PL");

	// Compile and create shaders
	//
	VkPipelineShaderStageCreateInfo vertexShader = {}, fragmentShader = {};
	res = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_VERTEX_BIT, VERTEX_SHADER_FILENAME, "main", "", &defines, &vertexShader);
	assert(res == VK_SUCCESS);
	res = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_SHADER_FILENAME, "main", "", &defines, &fragmentShader);
	assert(res == VK_SUCCESS);

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages = { vertexShader, fragmentShader };

	/////////////////////////////////////////////
	// Create pipeline
	
	// vertex input state

	std::vector<VkVertexInputBindingDescription> vi_binding(this->inputLayout.size());
	for (int i = 0; i < this->inputLayout.size(); i++)
	{
		vi_binding[i].binding = this->inputLayout[i].binding;
		vi_binding[i].stride = SizeOfFormat(this->inputLayout[i].format);
		vi_binding[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	}

	// input assembly state and layout

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.pNext = NULL;
	vi.flags = 0;
	vi.vertexBindingDescriptionCount = (uint32_t)vi_binding.size();
	vi.pVertexBindingDescriptions = vi_binding.data();
	vi.vertexAttributeDescriptionCount = (uint32_t)this->inputLayout.size();
	vi.pVertexAttributeDescriptions = this->inputLayout.data();

	VkPipelineInputAssemblyStateCreateInfo ia;
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.pNext = NULL;
	ia.flags = 0;
	ia.primitiveRestartEnable = VK_FALSE;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	// rasterizer state

	VkPipelineRasterizationStateCreateInfo rs;
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.pNext = NULL;
	rs.flags = 0;
	rs.polygonMode = VK_POLYGON_MODE_POINT;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.depthClampEnable = VK_FALSE;
	rs.rasterizerDiscardEnable = VK_FALSE;
	rs.depthBiasEnable = VK_FALSE;
	rs.depthBiasConstantFactor = 0;
	rs.depthBiasClamp = 0;
	rs.depthBiasSlopeFactor = 0;
	rs.lineWidth = 1.0f;

	VkPipelineColorBlendAttachmentState att_state[1];
	att_state[0].colorWriteMask = 0xf;
	att_state[0].blendEnable = VK_TRUE;
	att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
	att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
	att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

	// Color blend state

	VkPipelineColorBlendStateCreateInfo cb;
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.flags = 0;
	cb.pNext = NULL;
	cb.attachmentCount = 1;
	cb.pAttachments = att_state;
	cb.logicOpEnable = VK_FALSE;
	cb.logicOp = VK_LOGIC_OP_NO_OP;
	cb.blendConstants[0] = 1.0f;
	cb.blendConstants[1] = 1.0f;
	cb.blendConstants[2] = 1.0f;
	cb.blendConstants[3] = 1.0f;

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
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.stencilTestEnable = VK_FALSE;
	ds.back.failOp = VK_STENCIL_OP_KEEP;
	ds.back.passOp = VK_STENCIL_OP_KEEP;
	ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
	ds.back.compareMask = 0;
	ds.back.reference = 0;
	ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
	ds.back.writeMask = 0;
	ds.minDepthBounds = 0;
	ds.maxDepthBounds = 0;
	ds.front = ds.back;

	// multi sample state

	VkPipelineMultisampleStateCreateInfo ms;
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.pNext = NULL;
	ms.flags = 0;
	ms.pSampleMask = NULL;
	ms.rasterizationSamples = (VkSampleCountFlagBits)1;
	ms.sampleShadingEnable = VK_FALSE;
	ms.alphaToCoverageEnable = VK_FALSE;
	ms.alphaToOneEnable = VK_FALSE;
	ms.minSampleShading = 0.0;

	// create pipeline 

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

	res = vkCreateGraphicsPipelines(this->pDevice->GetDevice(), this->pDevice->GetPipelineCache(), 1, &pipeline, NULL, &this->pipeline);
	assert(res == VK_SUCCESS);
	SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)this->pipeline, "Caustics Mapping Pipeline");
}

void CausticsMapping::createReprojDescriptors(DefineList* pDefines)
{
	const uint32_t bindingCount = 4;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;

	//	define descriptor set
	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_Params"] = std::to_string(bindingIdx++);
	//	1. G-Buf depth (for world pos construction)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_GBufDepth"] = std::to_string(bindingIdx++);
	//	2. RSM depth
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMDepth"] = std::to_string(bindingIdx++);
	//	3. Caustics Map
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_CausticsMap"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->reproj_descriptorSetLayout,
		&this->reproj_descriptorSet);
}