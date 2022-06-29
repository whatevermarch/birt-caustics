#include "Caustics.h"

#include <random>

#define BLOCK_SIZE 16
#define MAX_PHOTON_COUNT (1u << 20) // 2e20 ~ 1M

void Caustics::OnCreate(
	Device* pDevice, 
	UploadHeap* pUploadHeap, 
	ResourceViewHeaps* pResourceViewHeaps,
	DynamicBufferRing* pDynamicBufferRing, 
	GBuffer* pRSM, VkImageView rsmDepthOpaque0SRV,
	Texture* pRSMDepthOpaque1N, int mipCount, 
	VkRenderPass renderPass)
{
	this->pDevice = pDevice;

	//	define render pass
	VkAttachmentDescription att_desc[1];
	AttachClearBeforeUse(
		VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
		(VkSampleCountFlagBits)1,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		&att_desc[0]);
	this->pm_renderPass = CreateRenderPassOptimal(this->pDevice->GetDevice(), 1, att_desc, nullptr);
#ifdef USE_BIRT
	this->rsmWidth = pRSM->m_EmissiveFlux.GetWidth() / 2;
	this->rsmHeight = pRSM->m_EmissiveFlux.GetHeight() / 2;
	
	this->pResourceViewHeaps = pResourceViewHeaps;
	this->pDynamicBufferRing = pDynamicBufferRing;

	//	define intermediate buffer storing photon tracing results
	{
		const uint32_t hitpointBufferMemSize = MAX_PHOTON_COUNT * sizeof(float) * 4;
		this->hitpointBuffer.OnCreateEx(this->pDevice, hitpointBufferMemSize, StaticBufferPool::STATIC_BUFFER_USAGE_GPU, "Hitpoint Buffer");

		bool res = this->hitpointBuffer.AllocBuffer(MAX_PHOTON_COUNT, sizeof(float) * 4, (void*)nullptr, &this->hitPosDescInfo);
		assert(res);
	}

	//	photon tracing pass
	{
		//  create default sampler
		this->sampler_default = CreateSampler(pDevice->GetDevice(), true);

		//  create sampler for depth mipmap
		this->sampler_depth = CreateSampler(pDevice->GetDevice(), false);

		//	create sampler for noise texture (sampling map)
		{
			VkSamplerCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = VK_FILTER_NEAREST;
			info.minFilter = VK_FILTER_NEAREST;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.unnormalizedCoordinates = VK_TRUE;
			info.minLod = -1000;
			info.maxLod = 1000;
			info.maxAnisotropy = 1.0f;
			VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_noise);
			assert(res == VK_SUCCESS);
		}

		//	generate smpling points
		this->generateSamplingPoints(*pUploadHeap);

		DefineList defines;

		// Create Descriptor Set (for each mip level we will create later on the individual Descriptor Sets)
		this->createPhotonTracerDescriptors(&defines);

		// Use helper class to create the compute pass
		this->photonTracer.OnCreate(this->pDevice, "PhotonTracer.glsl", "main", "", this->descriptorSetLayout, 0, 0, 0, &defines, sizeof(int));

		//	create image view for rsm depth (opaque)
		this->mipCount_rsm = mipCount;
		pRSMDepthOpaque1N->CreateSRV(&this->rsmDepthOpaque1NSRV);

		//	update desc set (except gbuf depth)
		this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(Caustics::Constants), this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 1, this->samplingMapSRV, &this->sampler_noise, this->descriptorSet);

		SetDescriptorSet(this->pDevice->GetDevice(), 2, pRSM->m_WorldCoordSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 3, pRSM->m_NormalBufferSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 4, pRSM->m_SpecularRoughnessSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 5, pRSM->m_EmissiveFluxSRV, &this->sampler_default, this->descriptorSet);

		SetDescriptorSetForDepth(this->pDevice->GetDevice(), 6, rsmDepthOpaque0SRV, &this->sampler_depth, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 7, this->rsmDepthOpaque1NSRV, &this->sampler_depth, this->descriptorSet);

		{
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.pNext = NULL;
			write.dstSet = this->descriptorSet;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			write.pBufferInfo = &this->hitPosDescInfo;
			write.dstBinding = 11;
			write.dstArrayElement = 0;

			vkUpdateDescriptorSets(this->pDevice->GetDevice(), 1, &write, 0, NULL);
		}
	}

	//	photon map (point rendering) pass
	{
		DefineList defines;

		// Create Pipeline
		this->createPhotonMapperPipeline(defines);
	}

	//	denoiser
	this->denoiser.OnCreate(pDevice, pResourceViewHeaps, pDynamicBufferRing);
#else
	//	use caustics mapping instead
	this->causticsMap.OnCreate(
		pDevice, pUploadHeap,
		pResourceViewHeaps,
		pDynamicBufferRing,
		pRSM->m_EmissiveFlux.GetWidth() / 2, 
		pRSM->m_EmissiveFlux.GetHeight() / 2,
		pRSM, rsmDepthOpaque0SRV,
		this->pm_renderPass
	);
#endif
}

void Caustics::OnDestroy()
{
#ifdef USE_BIRT
	//	denoiser
	this->denoiser.OnDestroy();

	//	photon map (point rendering) pass
	{
		vkDestroyPipeline(this->pDevice->GetDevice(), this->pm_pipeline, nullptr);
		vkDestroyPipelineLayout(this->pDevice->GetDevice(), this->pm_pipelineLayout, nullptr);
	}

	//	photon tracing pass
	{
		vkDestroyImageView(this->pDevice->GetDevice(), this->rsmDepthOpaque1NSRV, nullptr);

		this->photonTracer.OnDestroy();

		this->pResourceViewHeaps->FreeDescriptor(this->descriptorSet);
		vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayout, nullptr);

		vkDestroyImageView(this->pDevice->GetDevice(), this->samplingMapSRV, nullptr);
		this->samplingMap.OnDestroy();

		vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);
		vkDestroySampler(this->pDevice->GetDevice(), this->sampler_depth, nullptr);
		vkDestroySampler(this->pDevice->GetDevice(), this->sampler_noise, nullptr);
	}

	this->hitpointBuffer.OnDestroy();
	
	this->pResourceViewHeaps = nullptr;
	this->pDynamicBufferRing = nullptr;

	this->rsmWidth = 0;
	this->rsmHeight = 0;
#else
	this->causticsMap.OnDestroy();
#endif
	vkDestroyRenderPass(this->pDevice->GetDevice(), this->pm_renderPass, NULL);
	this->pm_renderPass = VK_NULL_HANDLE;

	this->pDevice = nullptr;
	this->pGPUTimeStamps = nullptr;
}

void Caustics::OnCreateWindowSizeDependentResources(
	uint32_t Width, uint32_t Height, 
	GBuffer* pGBuffer, 
	VkImageView gbufDepthOpaque0SRV, Texture* pGBufDepthOpaque1N, int mipCount)
{
	//	photon map (point rendering) pass
	{
		//	initialize render target
		this->pm_irradianceMap.InitRenderTarget(
			this->pDevice,
			Width, Height,
			VK_FORMAT_R16G16B16A16_SFLOAT/*VK_FORMAT_R16G16B16A16_UNORM*/,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			false,
			"Caustics Output"
		);
		this->pm_irradianceMap.CreateSRV(&this->pm_irradianceMapSRV);

		//	create framebuffer
		std::vector<VkImageView> attachments = {
			this->pm_irradianceMapSRV
		};

		this->pm_framebuffer = CreateFrameBuffer(
			this->pDevice->GetDevice(),
			this->pm_renderPass,
			&attachments,
			Width, Height
		);
	}
#ifdef USE_BIRT
	this->outWidth = Width;
	this->outHeight = Height;

	this->pGBuffer = pGBuffer;

	//	photon tracing pass
	{
		//	create image view for gbuf depth (opaque)
		this->mipCount_gbuf = mipCount;
		pGBufDepthOpaque1N->CreateSRV(&this->gbufDepthOpaque1NSRV);

		//	update desc set (only gbuf depth)
		SetDescriptorSetForDepth(this->pDevice->GetDevice(), 8, gbufDepthOpaque0SRV, &this->sampler_depth, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 9, this->gbufDepthOpaque1NSRV, &this->sampler_depth, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 10, pGBuffer->m_NormalBufferSRV, &this->sampler_default, this->descriptorSet);
	}

	//	denoiser
	this->denoiser.OnCreateWindowSizeDependentResources(
		this->outWidth, this->outHeight,
		&this->pm_irradianceMap, this->pm_irradianceMapSRV,
		gbufDepthOpaque0SRV, pGBuffer);
#else
	this->causticsMap.OnCreateWindowSizeDependentResources(
		Width, Height,
		pGBuffer, gbufDepthOpaque0SRV,
		this->pm_framebuffer);
#endif
}

void Caustics::OnDestroyWindowSizeDependentResources()
{
#ifdef USE_BIRT
	//	denoiser
	this->denoiser.OnDestroyWindowSizeDependentResources();

	//	photon tracing pass
	{
		vkDestroyImageView(this->pDevice->GetDevice(), this->gbufDepthOpaque1NSRV, nullptr);
	}

	this->pGBuffer = nullptr;
	this->outWidth = 0;
	this->outHeight = 0;
#else
	this->causticsMap.OnDestroyWindowSizeDependentResources();
#endif

	//	photon map (point rendering) pass
	{
		//  destroy frame buffer
		vkDestroyFramebuffer(this->pDevice->GetDevice(), this->pm_framebuffer, nullptr);
		this->pm_framebuffer = VK_NULL_HANDLE;

		//  destroy texture and its image view
		vkDestroyImageView(this->pDevice->GetDevice(), this->pm_irradianceMapSRV, nullptr);
		this->pm_irradianceMapSRV = VK_NULL_HANDLE;
		this->pm_irradianceMap.OnDestroy();
	}
}

void Caustics::Draw(VkCommandBuffer commandBuffer, const VkRect2D& renderArea, const Caustics::Constants& constants)
{
	SetPerfMarkerBegin(commandBuffer, "Caustics");
#ifdef USE_BIRT
	//	Opt.1 : BIRT Caustics
	//  update constants
	VkDescriptorBufferInfo descInfo_constants;
	{
		Caustics::Constants* pAllocData;
		this->pDynamicBufferRing->AllocConstantBuffer(sizeof(Caustics::Constants), (void**)&pAllocData, &descInfo_constants);
		*pAllocData = constants;
	}

	const uint32_t sampleDimPerBlock = BLOCK_SIZE * constants.samplingMapScale;
	const uint32_t numBlocks_x = (this->rsmWidth + sampleDimPerBlock - 1) / sampleDimPerBlock,
					numBlocks_y = (this->rsmHeight + sampleDimPerBlock - 1) / sampleDimPerBlock;
	const uint32_t numPhotons = BLOCK_SIZE * BLOCK_SIZE * numBlocks_x * numBlocks_y;

	//	photon tracing pass
	{
		SetPerfMarkerBegin(commandBuffer, "Photon Tracing");

		//  dispatch
		//
		this->photonTracer.Draw(commandBuffer, &descInfo_constants, this->descriptorSet, numBlocks_x, numBlocks_y, 1, &this->samplingSeed);
		this->samplingSeed = (this->samplingSeed + 1) % 8; // ToDo: need variable for 8

		this->pGPUTimeStamps->GetTimeStamp(commandBuffer, "BIRT: Photon Tracing");

		SetPerfMarkerEnd(commandBuffer);
	}

	//	photon map (point rendering) pass
	{
		//	start render pass
		VkClearValue cv{};
		cv.color = {0.f, 0.f, 0.f, 0.f};

		VkRenderPassBeginInfo rp_begin{};
		rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rp_begin.pNext = NULL;
		rp_begin.renderPass = this->pm_renderPass;
		rp_begin.framebuffer = this->pm_framebuffer;
		rp_begin.renderArea.offset.x = 0;
		rp_begin.renderArea.offset.y = 0;
		rp_begin.renderArea.extent.width = this->outWidth;
		rp_begin.renderArea.extent.height = this->outHeight;
		rp_begin.clearValueCount = 1;
		rp_begin.pClearValues = &cv;
		vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		SetPerfMarkerBegin(commandBuffer, "Photon Mapping");

		SetViewportAndScissor(commandBuffer, 
			renderArea.offset.x, renderArea.offset.y,
			renderArea.extent.width, renderArea.extent.height);

		// Bind vertices 
        //
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &hitPosDescInfo.buffer, &hitPosDescInfo.offset);

		// Bind Pipeline
        //
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pm_pipeline);

		// Draw
        //
		vkCmdDraw(commandBuffer, numPhotons, 1, 0, 0);

		SetPerfMarkerEnd(commandBuffer);

		//	end render pass
		vkCmdEndRenderPass(commandBuffer);

		this->pGPUTimeStamps->GetTimeStamp(commandBuffer, "BIRT: Photon Mapping");
	}

	//	denoising
	{
		SVGF::Constants svgfConst;
		svgfConst.alphaColor = 0.2f;
		svgfConst.alphaMoments = 0.2f;
		svgfConst.nearPlane = constants.camera.nearPlane;
		svgfConst.farPlane = constants.camera.farPlane;
		svgfConst.sigmaDepth = 1.f;
		svgfConst.sigmaNormal = 128.f;
		svgfConst.sigmaLuminance = 4.f;

		this->denoiser.Draw(commandBuffer, svgfConst);

		this->pGPUTimeStamps->GetTimeStamp(commandBuffer, "BIRT: Denoising");
	}
#else
	//	Opt.2 : Caustics Mapping
	{
		//	construct projection matrix
		//	ref : DirectXMathMatrix.inl -> XMMatrixPerspectiveFovRH()
		/*XMMATRIX proj{};
		XMFLOAT4 rowElems{};
		const float nearZ = constants.lights[0].nearPlane;
		const float farZ = constants.lights[0].farPlane;
		const float fRange = farZ / (nearZ - farZ);

		rowElems = { constants.lights[0].invTanHalfFovH, 0, 0, 0 };
		proj.r[0] = XMLoadFloat4(&rowElems);
		rowElems = { 0, constants.lights[0].invTanHalfFovV, 0, 0 };
		proj.r[1] = XMLoadFloat4(&rowElems);
		rowElems = { 0, 0, fRange, -1 };
		proj.r[2] = XMLoadFloat4(&rowElems);
		rowElems = { 0, 0, fRange * nearZ, 0 };
		proj.r[3] = XMLoadFloat4(&rowElems);*/

		const float nearZ = constants.lights[0].nearPlane;
		const float farZ = constants.lights[0].farPlane;
		const float fRange = farZ / (nearZ - farZ);

		//	setup constants
		CausticsMapping::Constants cmConst;
		cmConst.lightView = constants.lights[0].view;
		cmConst.lightInvTanHalfFovH = constants.lights[0].invTanHalfFovH;
		cmConst.lightInvTanHalfFovV = constants.lights[0].invTanHalfFovV;
		cmConst.lightFRange = fRange;
		cmConst.lightNearZ = nearZ;

		//	For cmConst.world -> it will be filled inside the CausticsMapping::Draw method.
		this->causticsMap.Draw(commandBuffer, renderArea, cmConst);

		this->pGPUTimeStamps->GetTimeStamp(commandBuffer, "Caustics Mapping");
	}
#endif
	SetPerfMarkerEnd(commandBuffer);
}
#ifdef USE_BIRT
void Caustics::createPhotonTracerDescriptors(DefineList* pDefines)
{
	const uint32_t bindingCount = 12;
	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
	uint32_t bindingIdx = 0;
	//	input
	//
	//	0. per-frame data
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_Params"] = std::to_string(bindingIdx++);
	//	1. Sampling map (noise texture)
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_SamplingMap"] = std::to_string(bindingIdx++);
	//	2. RSM world coord
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMWorldCoord"] = std::to_string(bindingIdx++);
	//	3. RSM normal
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMNormal"] = std::to_string(bindingIdx++);
	//	4. RSM specular/roughness
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMSpecular"] = std::to_string(bindingIdx++);
	//	5. RSM flux
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMFlux"] = std::to_string(bindingIdx++);
	//	6. RSM depth (opaque only) w/ mipmap lv.0
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMDepth_0"] = std::to_string(bindingIdx++);
	//	7. RSM depth (opaque only) w/ mipmap lv.1 to N
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_RSMDepth_1toN"] = std::to_string(bindingIdx++);
	//	8. GBuffer depth (opaque only) w/ mipmap lv.0
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_GBufDepth_0"] = std::to_string(bindingIdx++);
	//	9. GBuffer depth (opaque only) w/ mipmap lv.1-N
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_GBufDepth_1toN"] = std::to_string(bindingIdx++);
	//	10. GBuffer Normal
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_GBufNormal"] = std::to_string(bindingIdx++);

	//	output
	//
	//	11. HitPosition + Irradiance buffer
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	(*pDefines)["ID_HitPosIrradiance"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->descriptorSetLayout,
		&this->descriptorSet);
}

void Caustics::createPhotonMapperPipeline(const DefineList& defines)
{
	//	create pipeline layout
	//
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCreateInfo.pNext = NULL;
	pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
	pipelineLayoutCreateInfo.pPushConstantRanges = NULL;
	pipelineLayoutCreateInfo.setLayoutCount = 0;
	pipelineLayoutCreateInfo.pSetLayouts = NULL;

	VkResult res = vkCreatePipelineLayout(this->pDevice->GetDevice(), &pipelineLayoutCreateInfo, NULL, &this->pm_pipelineLayout);
	assert(res == VK_SUCCESS);
	SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)this->pm_pipelineLayout, "Caustics-PM PL");

	// Compile and create shaders
	//

	// Create the vertex shader
	static const char* str_vertexShader =
		"#version 450\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"#extension GL_ARB_shading_language_420pack : enable\n"
		"layout (location = 0) in vec4 in_hitPosAndPackedIrradiance;\n"
		"layout (location = 0) out float out_packedIrradiance;\n"
		"void main() {\n"
		"   out_packedIrradiance = in_hitPosAndPackedIrradiance.w;\n"
		"   gl_Position = vec4(in_hitPosAndPackedIrradiance.xyz, 1.0f);\n"
		"   gl_PointSize = 1.0f;\n"
		"}\n";

	// Create the fragment shader
	static const char* str_fragmentShader =
		"#version 450\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"#extension GL_ARB_shading_language_420pack : enable\n"
		"layout (location = 0) in float in_packedIrradiance;\n"
		"layout (location = 0) out vec4 out_irradiance;\n"
		//"#include \"RGBEConversion.h\"\n"
		"void main() {\n"
		"   if (in_packedIrradiance == 0)\n"
		"       discard;\n"
		//"   out_irradiance = vec4(RGBEToFloat3(floatBitsToUint(in_packedIrradiance)), 1.0f);\n"
		//	workaround: there's smth wrong with RGBE conversion
		"   out_irradiance = vec4(vec3(in_packedIrradiance), 1.0f);\n"
		"}\n";

	VkPipelineShaderStageCreateInfo vertexShader = {}, fragmentShader = {};
	res = VKCompileFromString(this->pDevice->GetDevice(), SST_GLSL, VK_SHADER_STAGE_VERTEX_BIT, str_vertexShader, "main", "", &defines, &vertexShader);
	assert(res == VK_SUCCESS);
	res = VKCompileFromString(this->pDevice->GetDevice(), SST_GLSL, VK_SHADER_STAGE_FRAGMENT_BIT, str_fragmentShader, "main", "", &defines, &fragmentShader);
	assert(res == VK_SUCCESS);

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages = { vertexShader, fragmentShader };

	// Create pipeline
	//

	/////////////////////////////////////////////
	// vertex input state

	VkVertexInputBindingDescription vi_bindings[1];
	vi_bindings[0] = {};
	vi_bindings[0].binding = 0;
	vi_bindings[0].stride = sizeof(float) * 4;
	vi_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription vi_attrs[] =
	{
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
	};

	// input assembly state and layout

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.pNext = NULL;
	vi.flags = 0;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = vi_bindings;
	vi.vertexAttributeDescriptionCount = _countof(vi_attrs);
	vi.pVertexAttributeDescriptions = vi_attrs;            

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
	pipeline.layout = this->pm_pipelineLayout;
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
	pipeline.renderPass = this->pm_renderPass;
	pipeline.subpass = 0;

	res = vkCreateGraphicsPipelines(this->pDevice->GetDevice(), this->pDevice->GetPipelineCache(), 1, &pipeline, NULL, &this->pm_pipeline);
	assert(res == VK_SUCCESS);
	SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)this->pm_pipeline, "Caustics-PM Pipeline");
}

void Caustics::generateSamplingPoints(UploadHeap& uploadHeap)
{
	//  initialize randomizer using uniform dist. w/ elem in [0,1]
	std::default_random_engine generator;
	std::uniform_real_distribution<float> distribution;

	//  setup sampling function
	auto sample_points = [&distribution, &generator]() -> std::pair<float, float>
	{ return { distribution(generator), distribution(generator) }; };

	//  generate noise texture in host
	const int noiseDim = BLOCK_SIZE;
	std::vector<std::pair<float,float>> noises(noiseDim * noiseDim);
	for (std::pair<float, float>& noise : noises)
		noise = sample_points();

	//  init noise texture object
	{
		IMG_INFO texInfo;
		texInfo.width = noiseDim;
		texInfo.height = noiseDim;
		texInfo.depth = 1;
		texInfo.mipMapCount = 1;
		texInfo.arraySize = 1;
		texInfo.format = DXGI_FORMAT_R32G32_FLOAT;
		texInfo.bitCount = 64;

		this->samplingMap.InitFromData(this->pDevice, uploadHeap, texInfo, noises.data(), "Sampling Map");
	}

	//  upload noise data to GPU
	uploadHeap.FlushAndFinish();

	//  create image view
	this->samplingMap.CreateSRV(&this->samplingMapSRV);
}
#endif