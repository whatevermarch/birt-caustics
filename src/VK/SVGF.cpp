#include "SVGF.h"

void SVGF::OnCreate(Device* pDevice, 
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing)
{
	this->pDevice = pDevice;
	this->pResourceViewHeaps = pResourceViewHeaps;
	this->pDynamicBufferRing = pDynamicBufferRing;

	//  create default sampler
	{
		VkSamplerCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_NEAREST;
		info.minFilter = VK_FILTER_NEAREST;
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

	//	temporal accumulation pass
	{
		//	create renderr pass
		//	render targets refer to intermediate buffers
		VkAttachmentDescription att_desc[3];
		AttachNoClearBeforeUse(
			VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			(VkSampleCountFlagBits)1,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			&att_desc[0]);
		AttachNoClearBeforeUse(
			VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			(VkSampleCountFlagBits)1,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			&att_desc[1]);
		AttachNoClearBeforeUse(
			VK_FORMAT_R8_UINT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			(VkSampleCountFlagBits)1,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			&att_desc[2]);
		this->ta_renderPass = CreateRenderPassOptimal(this->pDevice->GetDevice(), 3, att_desc, NULL);

		DefineList defines;
		this->createTADescriptors(defines);

		VkPipelineColorBlendAttachmentState att_state[3];
		att_state[0].colorWriteMask = 0xf;
		att_state[0].blendEnable = VK_FALSE;
		att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
		att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
		att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		att_state[1] = att_state[0];
		att_state[2] = att_state[0];

		VkPipelineColorBlendStateCreateInfo cb;
		cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		cb.flags = 0;
		cb.pNext = NULL;
		cb.attachmentCount = 3;
		cb.pAttachments = att_state;
		cb.logicOpEnable = VK_FALSE;
		cb.logicOp = VK_LOGIC_OP_NO_OP;
		cb.blendConstants[0] = 1.0f;
		cb.blendConstants[1] = 1.0f;
		cb.blendConstants[2] = 1.0f;
		cb.blendConstants[3] = 1.0f;
		this->tmpAccum.OnCreate(
			this->pDevice, 
			this->ta_renderPass,
			"SVGFReproject.glsl", "main", "", 
			NULL, pDynamicBufferRing, 
			this->ta_descriptorSetLayout, &cb, VK_SAMPLE_COUNT_1_BIT, 
			&defines);

		//	update descsciptors
		this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(SVGF::Constants), this->ta_descriptorSet);
	}

	//	variance estimation pass
	{
		DefineList defines;
		this->createVEDescriptors(defines);
		this->varEst.OnCreate(
			this->pDevice, 
			"SVGFStabilityBoost.glsl", "main", "", 
			this->ve_descriptorSetLayout, 0, 0, 0, 
			&defines);

		this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(SVGF::Constants), this->ve_descriptorSet);
	}

	//	a-trous wavelet transform pass
	{
		DefineList defines;
		this->createATDescriptors(defines);
		this->aTrous.OnCreate(
			this->pDevice, 
			"SVGFAtrousWT.glsl", "main", "", 
			this->at_descriptorSetLayout, 0, 0, 0, 
			&defines, sizeof(uint32_t));

		this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(SVGF::Constants), this->at_descriptorSet);
	}
}

void SVGF::OnDestroy()
{
	this->aTrous.OnDestroy();
	this->pResourceViewHeaps->FreeDescriptor(this->at_descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->at_descriptorSetLayout, nullptr);

	this->varEst.OnDestroy();
	this->pResourceViewHeaps->FreeDescriptor(this->ve_descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->ve_descriptorSetLayout, nullptr);

	this->tmpAccum.OnDestroy();
	vkDestroyRenderPass(this->pDevice->GetDevice(), this->ta_renderPass, nullptr);
	this->pResourceViewHeaps->FreeDescriptor(this->ta_descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->ta_descriptorSetLayout, nullptr);

	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);
}

void SVGF::OnCreateWindowSizeDependentResources(
	uint32_t Width, uint32_t Height,
	Texture* pTarget, VkImageView targetSRV,
	VkImageView depthSRV, GBuffer* pGBuffer)
{
	this->outWidth = Width;
	this->outHeight = Height;

	this->pInputHDR = pTarget;
	this->inputHDRSRV = targetSRV;
	this->pInputGBuffer = pGBuffer;

	//	cache buffer (previous frame)
	{
		this->cache_HDR.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"SVGF Cached HDR"
		);
		this->cache_HDR.CreateSRV(&this->cache_HDRSRV);

		this->cache_Normal.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"SVGF Cached Normal"
		);
		this->cache_Normal.CreateSRV(&this->cache_NormalSRV);

		this->cache_DepthMoment.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"SVGF Cached DepthMoment"
		);
		this->cache_DepthMoment.CreateSRV(&this->cache_DepthMomentSRV);

		this->cache_History.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R8_UINT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"SVGF Cached History"
		);
		this->cache_History.CreateSRV(&this->cache_HistorySRV);
	}

	//	intermediate buffer
	{
		this->imd_HDR.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"SVGF Intermediate HDR"
		);
		this->imd_HDR.CreateSRV(&this->imd_HDRSRV);

		this->imd_DepthMoment.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			false,
			"SVGF Intermediate DepthMoment"
		);
		this->imd_DepthMoment.CreateSRV(&this->imd_DepthMomentSRV);

		this->imd_History.InitRenderTarget(
			this->pDevice,
			this->outWidth, this->outHeight,
			VK_FORMAT_R8_UINT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			false,
			"SVGF Intermediate History"
		);
		this->imd_History.CreateSRV(&this->imd_HistorySRV);
	}

	//	update descriptor for each pass
	//	temporal accumulation
	{
		SetDescriptorSet(this->pDevice->GetDevice(), 1, targetSRV, &this->sampler_default, this->ta_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 2, pGBuffer->m_NormalBufferSRV, &this->sampler_default, this->ta_descriptorSet);
		SetDescriptorSetForDepth(this->pDevice->GetDevice(), 3, depthSRV, &this->sampler_default, this->ta_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 4, pGBuffer->m_MotionVectorsSRV, &this->sampler_default, this->ta_descriptorSet);

		SetDescriptorSet(this->pDevice->GetDevice(), 5, this->cache_HDRSRV, &this->sampler_default, this->ta_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 6, this->cache_NormalSRV, &this->sampler_default, this->ta_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 7, this->cache_DepthMomentSRV, &this->sampler_default, this->ta_descriptorSet);

		SetDescriptorSet(this->pDevice->GetDevice(), 8, this->cache_HistorySRV, &this->sampler_default, this->ta_descriptorSet);
	}

	//	variance estimation
	{
		SetDescriptorSet(this->pDevice->GetDevice(), 1, this->imd_HDRSRV, &this->sampler_default, this->ve_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 2, pGBuffer->m_NormalBufferSRV, &this->sampler_default, this->ve_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 3, this->imd_DepthMomentSRV, &this->sampler_default, this->ve_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 4, this->imd_HistorySRV, &this->sampler_default, this->ve_descriptorSet);

		//	for writable outputs
		VkDescriptorImageInfo imgInfos[4];
		VkWriteDescriptorSet writes[4];
		
		imgInfos[0].sampler = VK_NULL_HANDLE;
		imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		imgInfos[0].imageView = targetSRV;

		writes[0] = {};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].pNext = NULL;
		writes[0].dstSet = this->ve_descriptorSet;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[0].pImageInfo = &imgInfos[0];
		writes[0].dstBinding = 5;
		writes[0].dstArrayElement = 0;

		for (uint32_t i = 1; i < 4; i++)
		{
			imgInfos[i] = imgInfos[0];
			writes[i] = writes[0];
			writes[i].pImageInfo = &imgInfos[i];
		}
		imgInfos[1].imageView = this->cache_NormalSRV;
		writes[1].dstBinding = 6;
		imgInfos[2].imageView = this->cache_DepthMomentSRV;
		writes[2].dstBinding = 7;
		imgInfos[3].imageView = this->cache_HistorySRV;
		writes[3].dstBinding = 8;

		vkUpdateDescriptorSets(this->pDevice->GetDevice(), 4, writes, 0, NULL);
	}

	//	a-trous wavelet transform
	{
		SetDescriptorSet(this->pDevice->GetDevice(), 1, pGBuffer->m_NormalBufferSRV, &this->sampler_default, this->at_descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 2, this->imd_DepthMomentSRV, &this->sampler_default, this->at_descriptorSet);
		
		//	for writable outputs
		VkDescriptorImageInfo imgInfos[3];
		VkWriteDescriptorSet writes[3];

		imgInfos[0].sampler = VK_NULL_HANDLE;
		imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		imgInfos[0].imageView = this->inputHDRSRV;

		writes[0] = {};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].pNext = NULL;
		writes[0].dstSet = this->at_descriptorSet;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[0].pImageInfo = &imgInfos[0];
		writes[0].dstBinding = 3;
		writes[0].dstArrayElement = 0;

		imgInfos[1] = imgInfos[0];
		imgInfos[1].imageView = this->imd_HDRSRV;

		writes[1] = writes[0];
		writes[1].pImageInfo = &imgInfos[1];
		writes[1].dstBinding = 4;

		imgInfos[2] = imgInfos[0];
		imgInfos[2].imageView = this->cache_HDRSRV;

		writes[2] = writes[0];
		writes[2].pImageInfo = &imgInfos[2];
		writes[2].dstBinding = 5;

		vkUpdateDescriptorSets(this->pDevice->GetDevice(), 3, writes, 0, NULL);
	}

	//	create framebuffer (only for Temp Accum)
	std::vector<VkImageView> attachments = {
		this->imd_HDRSRV,
		this->imd_DepthMomentSRV,
		this->imd_HistorySRV
	};

	this->ta_framebuffer = CreateFrameBuffer(
		this->pDevice->GetDevice(),
		this->ta_renderPass,
		&attachments,
		this->outWidth, this->outHeight
	);
}

void SVGF::OnDestroyWindowSizeDependentResources()
{
	vkDestroyFramebuffer(this->pDevice->GetDevice(), this->ta_framebuffer, nullptr);
	this->ta_framebuffer = VK_NULL_HANDLE;

	//	intermediate buffer
	{
		vkDestroyImageView(this->pDevice->GetDevice(), this->imd_HistorySRV, nullptr);
		this->imd_HistorySRV = VK_NULL_HANDLE;
		this->imd_History.OnDestroy();

		vkDestroyImageView(this->pDevice->GetDevice(), this->imd_DepthMomentSRV, nullptr);
		this->imd_DepthMomentSRV = VK_NULL_HANDLE;
		this->imd_DepthMoment.OnDestroy();

		vkDestroyImageView(this->pDevice->GetDevice(), this->imd_HDRSRV, nullptr);
		this->imd_HDRSRV = VK_NULL_HANDLE;
		this->imd_HDR.OnDestroy();
	}

	//	cache buffer (previous frame)
	{
		vkDestroyImageView(this->pDevice->GetDevice(), this->cache_HistorySRV, nullptr);
		this->cache_HistorySRV = VK_NULL_HANDLE;
		this->cache_History.OnDestroy();

		vkDestroyImageView(this->pDevice->GetDevice(), this->cache_DepthMomentSRV, nullptr);
		this->cache_DepthMomentSRV = VK_NULL_HANDLE;
		this->cache_DepthMoment.OnDestroy();

		vkDestroyImageView(this->pDevice->GetDevice(), this->cache_NormalSRV, nullptr);
		this->cache_NormalSRV = VK_NULL_HANDLE;
		this->cache_Normal.OnDestroy();

		vkDestroyImageView(this->pDevice->GetDevice(), this->cache_HDRSRV, nullptr);
		this->cache_HDRSRV = VK_NULL_HANDLE;
		this->cache_HDR.OnDestroy();
	}

	this->pInputGBuffer = nullptr;
}

void SVGF::Draw(VkCommandBuffer commandBuffer, const SVGF::Constants& constants)
{
	SetPerfMarkerBegin(commandBuffer, "SVGF");

	//  update constants
	VkDescriptorBufferInfo descInfo_constants;
	{
		SVGF::Constants* pAllocData;
		this->pDynamicBufferRing->AllocConstantBuffer(sizeof(SVGF::Constants), (void**)&pAllocData, &descInfo_constants);
		*pAllocData = constants;
	}

	this->barrier_TA(commandBuffer);

	//	temporal accumulation pass
	{
		//  begin render pass
		{
			VkRenderPassBeginInfo rp_begin;
			rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			rp_begin.pNext = NULL;
			rp_begin.renderPass = this->ta_renderPass;
			rp_begin.framebuffer = this->ta_framebuffer;
			rp_begin.renderArea.offset.x = 0;
			rp_begin.renderArea.offset.y = 0;
			rp_begin.renderArea.extent.width = this->outWidth;
			rp_begin.renderArea.extent.height = this->outHeight;
			rp_begin.pClearValues = NULL;
			rp_begin.clearValueCount = 0;
			vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
		}

		SetPerfMarkerBegin(commandBuffer, "Temporal Accum");

		//  set viewport to be rendered
		SetViewportAndScissor(commandBuffer, 0, 0, this->outWidth, this->outHeight);

		this->tmpAccum.Draw(commandBuffer, &descInfo_constants, this->ta_descriptorSet);

		SetPerfMarkerEnd(commandBuffer);

		//  end render pass
		vkCmdEndRenderPass(commandBuffer);
	}

	const uint32_t numBlocks_x = (this->outWidth + 32 - 1) / 32,
					numBlocks_y = (this->outHeight + 32 - 1) / 32;

	this->barrier_VE(commandBuffer);

	//	variance estimation pass
	{
		SetPerfMarkerBegin(commandBuffer, "Variance Est");

		//  dispatch
		//
		this->varEst.Draw(commandBuffer, &descInfo_constants, this->ve_descriptorSet, numBlocks_x, numBlocks_y, 1);

		SetPerfMarkerEnd(commandBuffer);
	}

	this->barrier_AT(commandBuffer);

	//	a-trous wavelet transform pass
	{
		SetPerfMarkerBegin(commandBuffer, "A-trous WT");

		//	determine a-Trous level count
		const uint32_t iterCount = 4;
		
		//  dispatch
		//
		for (uint32_t i = 0; i < iterCount; i++)
		{
			this->barrier_AT_PerIter(commandBuffer, i);

			this->aTrous.Draw(commandBuffer, &descInfo_constants, this->at_descriptorSet, numBlocks_x, numBlocks_y, 1, &i);
		}

		SetPerfMarkerEnd(commandBuffer);
	}

	this->barrier_Out(commandBuffer);

	SetPerfMarkerEnd(commandBuffer);
}

void SVGF::createTADescriptors(DefineList& defines)
{
	const uint32_t bindingCount = 9;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;

	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_Params"] = std::to_string(bindingIdx++);
	//	1. Color Buffer (current frame)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_HDR"] = std::to_string(bindingIdx++);

	//	copy texture binding signature to the remaining
	for (uint32_t i = bindingIdx; i < bindingCount; i++)
	{
		layoutBindings[i] = layoutBindings[1];
		layoutBindings[i].binding = i;
	}

	//	2. Normal (current frame)
	defines["ID_Normal"] = std::to_string(bindingIdx++);
	//	3. Depth (current frame)
	defines["ID_Depth"] = std::to_string(bindingIdx++);
	//	4. Motion Vector (current frame)
	defines["ID_MotionVec"] = std::to_string(bindingIdx++);
	//	5. Color Buffer (previous frame)
	defines["ID_CacheHDR"] = std::to_string(bindingIdx++);
	//	6. Normal (previous frame)
	defines["ID_CacheNormal"] = std::to_string(bindingIdx++);
	//	7. Depth + Depth Gradient + Moments (L and L^2) (previous frame)
	defines["ID_CacheDepthMoment"] = std::to_string(bindingIdx++);
	//	8. History Buffer (previous frame)
	defines["ID_CacheHistory"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->ta_descriptorSetLayout,
		&this->ta_descriptorSet);
}

void SVGF::createVEDescriptors(DefineList& defines)
{
	const uint32_t bindingCount = 9;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;

	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_Params"] = std::to_string(bindingIdx++);
	//	1. Color Buffer (intermediate)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_InHDR"] = std::to_string(bindingIdx++);

	//	copy texture binding signature to the remaining
	for (uint32_t i = bindingIdx; i < 5; i++)
	{
		layoutBindings[i] = layoutBindings[1];
		layoutBindings[i].binding = i;
	}

	//	2. Normal (intermediate)
	defines["ID_Normal"] = std::to_string(bindingIdx++);
	//	3. Depth + Depth Gradient + Moments (L and L^2) (intermediate) 
	defines["ID_DepthMoment"] = std::to_string(bindingIdx++);
	//	4. History Buffer (intermediate)
	defines["ID_History"] = std::to_string(bindingIdx++);

	//	5. Color Buffer (target)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_OutHDR"] = std::to_string(bindingIdx++);

	//	copy storage image binding signature to the remaining
	for (uint32_t i = bindingIdx; i < bindingCount; i++)
	{
		layoutBindings[i] = layoutBindings[5];
		layoutBindings[i].binding = i;
	}

	//	6. Normal (target)
	defines["ID_CacheNormal"] = std::to_string(bindingIdx++);
	//	7. Depth + Depth Gradient + Moments (L and L^2) (target)
	defines["ID_CacheDepthMoment"] = std::to_string(bindingIdx++);
	//	8. History Buffer (target)
	defines["ID_CacheHistory"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->ve_descriptorSetLayout,
		&this->ve_descriptorSet);
}

void SVGF::createATDescriptors(DefineList& defines)
{
	const uint32_t bindingCount = 6;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;

	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_Params"] = std::to_string(bindingIdx++);

	//	1. Normal (intermediate) 
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_Normal"] = std::to_string(bindingIdx++);
	//	2. Depth + Depth Gradient + Moments (L and L^2) (intermediate) 
	layoutBindings[bindingIdx] = layoutBindings[1];
	layoutBindings[bindingIdx].binding = bindingIdx;
	defines["ID_DepthMoment"] = std::to_string(bindingIdx++);

	//	3. Color Buffer (target)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_InHDR"] = std::to_string(bindingIdx++);

	//	copy storage image binding signature to the remaining
	for (uint32_t i = bindingIdx; i < bindingCount; i++)
	{
		layoutBindings[i] = layoutBindings[3];
		layoutBindings[i].binding = i;
	}

	//	4. Color Buffer (intermediate)
	defines["ID_ImdHDR"] = std::to_string(bindingIdx++);
	//	5. Color Buffer (cache. only used in 1st iter.)
	defines["ID_CacheHDR"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->at_descriptorSetLayout,
		&this->at_descriptorSet);
}

void SVGF::setATInOutHDR(VkCommandBuffer cmdBuf, uint32_t atrousIter)
{
	bool toInput = (atrousIter % 2 != 0);

	//	transition input & output
	//
	const uint32_t numBarriers = 2;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;

	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : in
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = toInput ? this->imd_HDR.Resource() : this->pInputHDR->Resource();

	//  barrier 1 : out
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx++].image = toInput ? this->pInputHDR->Resource() : this->imd_HDR.Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		numBarriers, barriers);

	//	update descriptor
	//
	VkDescriptorImageInfo imgInfos[2];
	VkWriteDescriptorSet writes[2];

	//	set input
	imgInfos[0].sampler = this->sampler_default;
	imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgInfos[0].imageView = toInput ? this->imd_HDRSRV : this->inputHDRSRV;

	writes[0] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].pNext = NULL;
	writes[0].dstSet = this->at_descriptorSet;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &imgInfos[0];
	writes[0].dstBinding = 1;
	writes[0].dstArrayElement = 0;

	//	set output
	imgInfos[1].sampler = VK_NULL_HANDLE;
	imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	imgInfos[1].imageView = toInput ? this->inputHDRSRV : this->imd_HDRSRV;

	writes[1] = {};
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].pNext = NULL;
	writes[1].dstSet = this->at_descriptorSet;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &imgInfos[1];
	writes[1].dstBinding = 4;
	writes[1].dstArrayElement = 0;

	vkUpdateDescriptorSets(this->pDevice->GetDevice(), 2, writes, 0, NULL);
}

void SVGF::barrier_TA(VkCommandBuffer cmdBuf)
{
	//	transition caches
	//
	const uint32_t numBarriers = 7;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;

	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : color buffer
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = this->cache_HDR.Resource();

	//  barrier 1 : normal
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_Normal.Resource();

	//  barrier 2 : dpeht + moment
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_DepthMoment.Resource();

	//  barrier 3 : history
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_History.Resource();

	assert(barrierIdx == 4);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		4, barriers);

	//	transition intermediate buffers
	//
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	//  barrier 4 : color buffer
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[barrierIdx++].image = this->imd_HDR.Resource();

	//  barrier 5 : depth + moment
	barriers[barrierIdx] = barriers[4];
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx++].image = this->imd_DepthMoment.Resource();

	//  barrier 6 : history
	barriers[barrierIdx] = barriers[5];
	barriers[barrierIdx++].image = this->imd_History.Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL,
		3, barriers + 4);
}

void SVGF::barrier_VE(VkCommandBuffer cmdBuf)
{
	//	transition input HDR + cache
	//
	const uint32_t numBarriers = 4;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;
	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : color buffer (input)
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = this->pInputHDR->Resource();

	//  barrier 1 : normal
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_Normal.Resource();

	//  barrier 2 : depth + moment
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_DepthMoment.Resource();

	//  barrier 3 : history
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx++].image = this->cache_History.Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		numBarriers, barriers);
}

void SVGF::barrier_AT(VkCommandBuffer cmdBuf)
{
	//	transition intermediate buffer + cache
	//
	const uint32_t numBarriers = 1;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;
	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : color buffer (cache)
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = this->cache_HDR.Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		numBarriers, barriers);
}

void SVGF::barrier_AT_PerIter(VkCommandBuffer cmdBuf, uint32_t atrousIter)
{
	//  determine if the output for this a-trous iteration is the input color buffer itself.
	bool toInput = (atrousIter & 1) != 0;

	//	transition input & output
	//
	const uint32_t numBarriers = 2;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;

	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : in
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = toInput ? this->imd_HDR.Resource() : this->pInputHDR->Resource();

	//  barrier 1 : out
	barriers[barrierIdx] = barriers[0];
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].oldLayout = (atrousIter == 0) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx++].image = toInput ? this->pInputHDR->Resource() : this->imd_HDR.Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		numBarriers, barriers);
}

void SVGF::barrier_Out(VkCommandBuffer cmdBuf)
{
	//	transition output (input itself)
	//
	const uint32_t numBarriers = 1;
	VkImageMemoryBarrier barriers[numBarriers];
	uint32_t barrierIdx = 0;
	barriers[barrierIdx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[barrierIdx].pNext = NULL;
	barriers[barrierIdx].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[barrierIdx].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[barrierIdx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[barrierIdx].subresourceRange.baseMipLevel = 0;
	barriers[barrierIdx].subresourceRange.levelCount = 1;
	barriers[barrierIdx].subresourceRange.baseArrayLayer = 0;
	barriers[barrierIdx].subresourceRange.layerCount = 1;

	//  barrier 0 : color buffer
	barriers[barrierIdx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[barrierIdx].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[barrierIdx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[barrierIdx++].image = this->pInputHDR->Resource();

	assert(barrierIdx == numBarriers);
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		numBarriers, barriers);
}