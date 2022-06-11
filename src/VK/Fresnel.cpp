#include "Fresnel.h"

#include <random>

#define BLOCK_SIZE 16

void Fresnel::OnCreate(
	Device* pDevice, 
	UploadHeap* pUploadHeap, 
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing, 
	VkRenderPass renderPass)
{
	this->pDevice = pDevice;
	this->pResourceViewHeaps = pResourceViewHeaps;
	this->pDynamicBufferRing = pDynamicBufferRing;

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
	this->createDescriptors(defines);

	// Use helper class to create the compute pass
	this->pathTracer.OnCreate(this->pDevice, "PathTracer.glsl", "main", "", this->descriptorSetLayout, 
		0, 0, 0, &defines, sizeof(int));

	//	update desc set (except gbuf depth)
	this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(Fresnel::Constants), this->descriptorSet);
	SetDescriptorSet(this->pDevice->GetDevice(), 1, this->samplingMapSRV, &this->sampler_noise, this->descriptorSet);

	//	denoiser
	this->denoiser.OnCreate(pDevice, pResourceViewHeaps, pDynamicBufferRing);
}

void Fresnel::OnDestroy()
{
	this->denoiser.OnDestroy();

	this->pathTracer.OnDestroy();

	this->pResourceViewHeaps->FreeDescriptor(this->descriptorSet);
	vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayout, nullptr);

	vkDestroyImageView(this->pDevice->GetDevice(), this->samplingMapSRV, nullptr);
	this->samplingMap.OnDestroy();

	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);
	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_depth, nullptr);
	vkDestroySampler(this->pDevice->GetDevice(), this->sampler_noise, nullptr);
}

void Fresnel::OnCreateWindowSizeDependentResources(
	uint32_t Width, uint32_t Height, 
	GBuffer* pGBuffer, VkImageView gbufDepthOpaque0SRV, 
	Texture* pGBufDepthOpaque1N, 
	VkImageView opaqueHDRSRV)
{
	this->outWidth = Width;
	this->outHeight = Height;

	this->pGBuffer = pGBuffer;

	//	create render target
	this->radianceMap.InitRenderTarget(
		this->pDevice,
		this->outWidth, this->outHeight,
		VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		false,
		"Fresnel Radiance Map"
	);
	this->radianceMap.CreateSRV(&this->radianceMapSRV);

	//	switch image layout to GENERAL (for image write)


	//	path tracing pass
	{
		//	create image view for gbuf depth (opaque)
		pGBufDepthOpaque1N->CreateSRV(&this->gbufDepthOpaque1NSRV);

		//	update desc set (only gbuf depth)
		SetDescriptorSet(this->pDevice->GetDevice(), 2, pGBuffer->m_WorldCoordSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 3, pGBuffer->m_NormalBufferSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 4, pGBuffer->m_SpecularRoughnessSRV, &this->sampler_default, this->descriptorSet);
		SetDescriptorSetForDepth(this->pDevice->GetDevice(), 5, gbufDepthOpaque0SRV, &this->sampler_depth, this->descriptorSet);
		SetDescriptorSet(this->pDevice->GetDevice(), 6, this->gbufDepthOpaque1NSRV, &this->sampler_depth, this->descriptorSet);

		//	create image view for opaque-only final color
		SetDescriptorSet(this->pDevice->GetDevice(), 7, opaqueHDRSRV, &this->sampler_depth, this->descriptorSet);

		{
			VkDescriptorImageInfo imgInfo;
			imgInfo.sampler = VK_NULL_HANDLE;
			imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			imgInfo.imageView = this->radianceMapSRV;

			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.pNext = NULL;
			write.dstSet = this->descriptorSet;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			write.pImageInfo = &imgInfo;
			write.dstBinding = 8;
			write.dstArrayElement = 0;

			vkUpdateDescriptorSets(this->pDevice->GetDevice(), 1, &write, 0, NULL);
		}
	}

	//	denoiser
	this->denoiser.OnCreateWindowSizeDependentResources(
		this->outWidth, this->outHeight,
		&this->radianceMap, this->radianceMapSRV,
		gbufDepthOpaque0SRV, pGBuffer);
}

void Fresnel::OnDestroyWindowSizeDependentResources()
{
	//	destroy denoiser
	this->denoiser.OnDestroyWindowSizeDependentResources();

	//  destroy texture and its image view
	vkDestroyImageView(this->pDevice->GetDevice(), this->radianceMapSRV, nullptr);
	this->radianceMapSRV = VK_NULL_HANDLE;
	this->radianceMap.OnDestroy();

	vkDestroyImageView(this->pDevice->GetDevice(), this->gbufDepthOpaque1NSRV, nullptr);
}

void Fresnel::Draw(VkCommandBuffer commandBuffer, 
	const VkRect2D& renderArea,
	const Fresnel::Constants& constants)
{
	SetPerfMarkerBegin(commandBuffer, "Fresnel");

	//  update constants
	VkDescriptorBufferInfo descInfo_constants;
	{
		Fresnel::Constants* pAllocData;
		this->pDynamicBufferRing->AllocConstantBuffer(sizeof(Fresnel::Constants), (void**)&pAllocData, &descInfo_constants);
		*pAllocData = constants;
	}

	const uint32_t sampleDimPerBlock = BLOCK_SIZE * constants.samplingMapScale;
	const uint32_t numBlocks_x = (this->outWidth + sampleDimPerBlock - 1) / sampleDimPerBlock,
		numBlocks_y = (this->outHeight + sampleDimPerBlock - 1) / sampleDimPerBlock;
	const uint32_t numPhotons = BLOCK_SIZE * BLOCK_SIZE * numBlocks_x * numBlocks_y;

	this->barrier_In(commandBuffer);

	//	path tracing pass
	{
		SetPerfMarkerBegin(commandBuffer, "Path Tracing");

		//	clear old color first, since it is standalone compute shader 
		//	(not fragment shader that has clear mechanism under render pass)
		const VkClearColorValue clearVal{ 0.f, 0.f, 0.f, 1.f };
		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel = 0;
		range.levelCount = 1;
		range.baseArrayLayer = 0;
		range.layerCount = 1;
		vkCmdClearColorImage(commandBuffer, this->radianceMap.Resource(), VK_IMAGE_LAYOUT_GENERAL, &clearVal, 1, &range);

		//  dispatch
		//
		this->pathTracer.Draw(commandBuffer, &descInfo_constants, this->descriptorSet, numBlocks_x, numBlocks_y, 1, &this->samplingSeed);
		this->samplingSeed = (this->samplingSeed + 1) % 8; // ToDo: need variable for 8

		SetPerfMarkerEnd(commandBuffer);
	}

	this->barrier_Out(commandBuffer);

	//	denoise
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
	}

	SetPerfMarkerEnd(commandBuffer);
}

void Fresnel::createDescriptors(DefineList& defines)
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
	//	1. Sampling map
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_SamplingMap"] = std::to_string(bindingIdx++);

	//	copy texture binding signature to the remaining
	for (uint32_t i = bindingIdx; i < bindingCount - 1; i++)
	{
		layoutBindings[i] = layoutBindings[1];
		layoutBindings[i].binding = i;
	}

	//	2. World Coord
	defines["ID_GBufWorldCoord"] = std::to_string(bindingIdx++);
	//	3. Normal
	defines["ID_GBufNormal"] = std::to_string(bindingIdx++);
	//	4. Specular-Roughness
	defines["ID_GBufSpecular"] = std::to_string(bindingIdx++);
	//	5. Depth lv 0
	defines["ID_GBufDepth_0"] = std::to_string(bindingIdx++);
	//	6. Depth lv 1-N
	defines["ID_GBufDepth_1toN"] = std::to_string(bindingIdx++);

	//	7. Opaque-only render result
	defines["ID_BackColor"] = std::to_string(bindingIdx++);

	//	8. Target buffer
	layoutBindings[bindingIdx].binding = bindingIdx;
	layoutBindings[bindingIdx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	layoutBindings[bindingIdx].descriptorCount = 1;
	layoutBindings[bindingIdx].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layoutBindings[bindingIdx].pImmutableSamplers = NULL;
	defines["ID_Target"] = std::to_string(bindingIdx++);

	assert(bindingIdx == bindingCount);
	this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
		&layoutBindings,
		&this->descriptorSetLayout,
		&this->descriptorSet);
}

void Fresnel::generateSamplingPoints(UploadHeap& uploadHeap)
{
	const int noiseDim = BLOCK_SIZE;
	std::vector<std::pair<char, char>> noises(noiseDim * noiseDim);

	//	workaround : fix the image properties
	const uint32_t bytePerPixel = 4;

	//	buffer for loaded image file
	char texData[noiseDim * noiseDim * bytePerPixel];

	//	use blue noise from file
	IMG_INFO img_header;
	static const auto noiseFile_r = "..\\res\\BlueNoise\\noise_r.png";
	static const auto noiseFile_g = "..\\res\\BlueNoise\\noise_g.png";

	//	load r-component first
	ImgLoader* img = CreateImageLoader(noiseFile_r);
	bool result = img->Load(noiseFile_r, 1.f, &img_header);
	if (result)
	{
		img->CopyPixels(texData, img_header.width * bytePerPixel, img_header.width * bytePerPixel, img_header.height);

		//	pick only one channel element to be our coordinate
		char* pixItr = texData;
		for (std::pair<char, char>& noise : noises)
		{
			//	R-component
			noise.first = (*pixItr);
			pixItr += bytePerPixel;
		}
	}
	delete(img);

	//	then load g-component
	img = CreateImageLoader(noiseFile_g);
	result = img->Load(noiseFile_g, 1.f, &img_header);
	if (result)
	{
		img->CopyPixels(texData, img_header.width * bytePerPixel, img_header.width * bytePerPixel, img_header.height);

		//	pick only one channel element to be our coordinate
		char* pixItr = texData;
		for (std::pair<char, char>& noise : noises)
		{
			//	G-component
			noise.second = (*pixItr);
			pixItr += bytePerPixel;
		}
	}
	delete(img);

	//OutputDebugStringW(L"My output string.");

	////  initialize randomizer using uniform dist. w/ elem in [0,1]
	//std::default_random_engine generator;
	//std::uniform_real_distribution<float> distribution;

	////  setup sampling function
	//auto sample_points = [&distribution, &generator]() -> std::pair<float, float>
	//{ return { distribution(generator), distribution(generator) }; };

	////  generate noise texture in host
	//const int noiseDim = BLOCK_SIZE;
	//std::vector<std::pair<float, float>> noises(noiseDim * noiseDim);
	//for (std::pair<float, float>& noise : noises)
	//	noise = sample_points();

	//	noise data is ready, initialize image object!
	{
		IMG_INFO texInfo;
		texInfo.width = noiseDim;
		texInfo.height = noiseDim;
		texInfo.depth = 1;
		texInfo.mipMapCount = 1;
		texInfo.arraySize = 1;
		texInfo.format = DXGI_FORMAT_R8G8_UNORM;
		texInfo.bitCount = 16;

		this->samplingMap.InitFromData(this->pDevice, uploadHeap, texInfo, noises.data(), "Blue-Noise Sampling Map");
	}

	//  upload noise data to GPU
	uploadHeap.FlushAndFinish();

	//this->samplingMap.InitFromFile(pDevice, &uploadHeap, "blue_noise.png", false);
	this->samplingMap.CreateSRV(&this->samplingMapSRV);
}

void Fresnel::barrier_In(VkCommandBuffer cmdBuf)
{
	VkImageMemoryBarrier barriers[1];
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].pNext = NULL;
	barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; // VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].subresourceRange.baseMipLevel = 0;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.baseArrayLayer = 0;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].image = this->radianceMap.Resource();

	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		1, barriers);
}

void Fresnel::barrier_Out(VkCommandBuffer cmdBuf)
{
	VkImageMemoryBarrier barriers[1];
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].pNext = NULL;
	barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT; // VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].subresourceRange.baseMipLevel = 0;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.baseArrayLayer = 0;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].image = this->radianceMap.Resource();

	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL,
		1, barriers);
}