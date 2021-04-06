/*
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "dll_log.hpp"
#include "hook_manager.hpp"
#include "vulkan_hooks.hpp"
#include "lockfree_table.hpp"
#include "runtime_vk.hpp"
#include "render_vk_utils.hpp"
#include "format_utils.hpp"

lockfree_table<void *, reshade::vulkan::device_impl *, 16> g_vulkan_devices;
static lockfree_table<VkQueue, reshade::vulkan::command_queue_impl *, 16> s_vulkan_queues;
static lockfree_table<VkCommandBuffer, reshade::vulkan::command_list_impl *, 4096> s_vulkan_command_buffers;
extern lockfree_table<void *, VkLayerInstanceDispatchTable, 16> g_instance_dispatch;
extern lockfree_table<VkSurfaceKHR, HWND, 16> g_surface_windows;
static lockfree_table<VkSwapchainKHR, reshade::vulkan::runtime_impl *, 16> s_vulkan_runtimes;

#define GET_DISPATCH_PTR(name, object) \
	GET_DISPATCH_PTR_FROM(name, g_vulkan_devices.at(dispatch_key_from_handle(object)))
#define GET_DISPATCH_PTR_FROM(name, data) \
	assert((data) != nullptr); \
	PFN_vk##name trampoline = (data)->_dispatch_table.name; \
	assert(trampoline != nullptr)
#define INIT_DISPATCH_PTR(name) \
	dispatch_table.name = reinterpret_cast<PFN_vk##name>(get_device_proc(device, "vk" #name))

VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	LOG(INFO) << "Redirecting " << "vkCreateDevice" << '(' << "physicalDevice = " << physicalDevice << ", pCreateInfo = " << pCreateInfo << ", pAllocator = " << pAllocator << ", pDevice = " << pDevice << ')' << " ...";

	assert(pCreateInfo != nullptr && pDevice != nullptr);

	// Look for layer link info if installed as a layer (provided by the Vulkan loader)
	VkLayerDeviceCreateInfo *const link_info = find_layer_info<VkLayerDeviceCreateInfo>(
		pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);

	// Get trampoline function pointers
	PFN_vkCreateDevice trampoline = nullptr;
	PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
	PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;

	if (link_info != nullptr)
	{
		assert(link_info->u.pLayerInfo != nullptr);
		assert(link_info->u.pLayerInfo->pfnNextGetDeviceProcAddr != nullptr);
		assert(link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr != nullptr);

		// Look up functions in layer info
		get_device_proc = link_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
		get_instance_proc = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
		trampoline = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc(nullptr, "vkCreateDevice"));

		// Advance the link info for the next element on the chain
		link_info->u.pLayerInfo = link_info->u.pLayerInfo->pNext;
	}
#ifdef RESHADE_TEST_APPLICATION
	else
	{
		trampoline = reshade::hooks::call(vkCreateDevice);
		get_device_proc = reshade::hooks::call(vkGetDeviceProcAddr);
		get_instance_proc = reshade::hooks::call(vkGetInstanceProcAddr);
	}
#endif

	if (trampoline == nullptr) // Unable to resolve next 'vkCreateDevice' function in the call chain
		return VK_ERROR_INITIALIZATION_FAILED;

	LOG(INFO) << "> Dumping enabled device extensions:";
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
		LOG(INFO) << "  " << pCreateInfo->ppEnabledExtensionNames[i];

	auto enum_queue_families = g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)).GetPhysicalDeviceQueueFamilyProperties;
	assert(enum_queue_families != nullptr);
	auto enum_device_extensions = g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)).EnumerateDeviceExtensionProperties;
	assert(enum_device_extensions != nullptr);

	uint32_t num_queue_families = 0;
	enum_queue_families(physicalDevice, &num_queue_families, nullptr);
	std::vector<VkQueueFamilyProperties> queue_families(num_queue_families);
	enum_queue_families(physicalDevice, &num_queue_families, queue_families.data());

	uint32_t graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0, queue_family_index; i < pCreateInfo->queueCreateInfoCount; ++i)
	{
		queue_family_index = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
		assert(queue_family_index < num_queue_families);

		// Find the first queue family which supports graphics and has at least one queue
		if (pCreateInfo->pQueueCreateInfos[i].queueCount > 0 && (queue_families[queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			if (pCreateInfo->pQueueCreateInfos[i].pQueuePriorities[0] < 1.0f)
				LOG(WARN) << "Vulkan queue used for rendering has a low priority (" << pCreateInfo->pQueueCreateInfos[i].pQueuePriorities[0] << ").";

			graphics_queue_family_index = queue_family_index;
			break;
		}
	}

	VkPhysicalDeviceFeatures enabled_features = {};
	const VkPhysicalDeviceFeatures2 *features2 = find_in_structure_chain<VkPhysicalDeviceFeatures2>(
		pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
	if (features2 != nullptr) // The features from the structure chain take precedence
		enabled_features = features2->features;
	else if (pCreateInfo->pEnabledFeatures != nullptr)
		enabled_features = *pCreateInfo->pEnabledFeatures;

	std::vector<const char *> enabled_extensions;
	enabled_extensions.reserve(pCreateInfo->enabledExtensionCount);
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
		enabled_extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);

	// Check if the device is used for presenting
	if (std::find_if(enabled_extensions.begin(), enabled_extensions.end(),
		[](const char *name) { return strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }) == enabled_extensions.end())
	{
		LOG(WARN) << "Skipping device because it is not created with the \"" VK_KHR_SWAPCHAIN_EXTENSION_NAME "\" extension.";

		graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
	}
	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	else if (graphics_queue_family_index == std::numeric_limits<uint32_t>::max())
	{
		LOG(WARN) << "Skipping device because it is not created with a graphics queue.";
	}
	else
	{
		uint32_t num_extensions = 0;
		enum_device_extensions(physicalDevice, nullptr, &num_extensions, nullptr);
		std::vector<VkExtensionProperties> extensions(num_extensions);
		enum_device_extensions(physicalDevice, nullptr, &num_extensions, extensions.data());

		// Make sure the driver actually supports the requested extensions
		const auto add_extension = [&extensions, &enabled_extensions, &graphics_queue_family_index](const char *name, bool required) {
			if (const auto it = std::find_if(extensions.begin(), extensions.end(),
				[name](const auto &props) { return strncmp(props.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0; });
				it != extensions.end())
			{
				enabled_extensions.push_back(name);
				return true;
			}

			if (required)
			{
				LOG(ERROR) << "Required extension \"" << name << "\" is not supported on this device. Initialization failed.";

				// Reset queue family index to prevent ReShade initialization
				graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
			}
			else
			{
				LOG(WARN)  << "Optional extension \"" << name << "\" is not supported on this device.";
			}

			return false;
		};

		// Enable features that ReShade requires
		enabled_features.shaderImageGatherExtended = true;
		enabled_features.shaderStorageImageWriteWithoutFormat = true;

		// Enable extensions that ReShade requires
		add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, false); // This is optional, see imgui code in 'runtime_impl'
		add_extension(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, true);
		add_extension(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, true);
#ifndef NDEBUG
		add_extension(VK_EXT_DEBUG_MARKER_EXTENSION_NAME, false);
#endif
	}

	VkDeviceCreateInfo create_info = *pCreateInfo;
	create_info.enabledExtensionCount = uint32_t(enabled_extensions.size());
	create_info.ppEnabledExtensionNames = enabled_extensions.data();

	// Patch the enabled features
	if (features2 != nullptr)
		// This is evil, because overwriting application memory, but whatever (RenderDoc does this too)
		const_cast<VkPhysicalDeviceFeatures2 *>(features2)->features = enabled_features;
	else
		create_info.pEnabledFeatures = &enabled_features;

	// Continue calling down the chain
	const VkResult result = trampoline(physicalDevice, &create_info, pAllocator, pDevice);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateDevice" << " failed with error code " << result << '.';
		return result;
	}

	VkDevice device = *pDevice;
	// Initialize the device dispatch table
	VkLayerDispatchTable dispatch_table = { get_device_proc };

	// ---- Core 1_0 commands
	INIT_DISPATCH_PTR(DestroyDevice);
	INIT_DISPATCH_PTR(GetDeviceQueue);
	INIT_DISPATCH_PTR(QueueSubmit);
	INIT_DISPATCH_PTR(QueueWaitIdle);
	INIT_DISPATCH_PTR(DeviceWaitIdle);
	INIT_DISPATCH_PTR(AllocateMemory);
	INIT_DISPATCH_PTR(FreeMemory);
	INIT_DISPATCH_PTR(MapMemory);
	INIT_DISPATCH_PTR(UnmapMemory);
	INIT_DISPATCH_PTR(FlushMappedMemoryRanges);
	INIT_DISPATCH_PTR(InvalidateMappedMemoryRanges);
	INIT_DISPATCH_PTR(BindBufferMemory);
	INIT_DISPATCH_PTR(BindImageMemory);
	INIT_DISPATCH_PTR(GetBufferMemoryRequirements);
	INIT_DISPATCH_PTR(GetImageMemoryRequirements);
	INIT_DISPATCH_PTR(CreateFence);
	INIT_DISPATCH_PTR(DestroyFence);
	INIT_DISPATCH_PTR(ResetFences);
	INIT_DISPATCH_PTR(GetFenceStatus);
	INIT_DISPATCH_PTR(WaitForFences);
	INIT_DISPATCH_PTR(CreateSemaphore);
	INIT_DISPATCH_PTR(DestroySemaphore);
	INIT_DISPATCH_PTR(CreateQueryPool);
	INIT_DISPATCH_PTR(DestroyQueryPool);
	INIT_DISPATCH_PTR(GetQueryPoolResults);
	INIT_DISPATCH_PTR(CreateBuffer);
	INIT_DISPATCH_PTR(DestroyBuffer);
	INIT_DISPATCH_PTR(CreateBufferView);
	INIT_DISPATCH_PTR(DestroyBufferView);
	INIT_DISPATCH_PTR(CreateImage);
	INIT_DISPATCH_PTR(DestroyImage);
	INIT_DISPATCH_PTR(GetImageSubresourceLayout);
	INIT_DISPATCH_PTR(CreateImageView);
	INIT_DISPATCH_PTR(DestroyImageView);
	INIT_DISPATCH_PTR(CreateShaderModule);
	INIT_DISPATCH_PTR(DestroyShaderModule);
	INIT_DISPATCH_PTR(CreateGraphicsPipelines);
	INIT_DISPATCH_PTR(CreateComputePipelines);
	INIT_DISPATCH_PTR(DestroyPipeline);
	INIT_DISPATCH_PTR(CreatePipelineLayout);
	INIT_DISPATCH_PTR(DestroyPipelineLayout);
	INIT_DISPATCH_PTR(CreateSampler);
	INIT_DISPATCH_PTR(DestroySampler);
	INIT_DISPATCH_PTR(CreateDescriptorSetLayout);
	INIT_DISPATCH_PTR(DestroyDescriptorSetLayout);
	INIT_DISPATCH_PTR(CreateDescriptorPool);
	INIT_DISPATCH_PTR(DestroyDescriptorPool);
	INIT_DISPATCH_PTR(ResetDescriptorPool);
	INIT_DISPATCH_PTR(AllocateDescriptorSets);
	INIT_DISPATCH_PTR(FreeDescriptorSets);
	INIT_DISPATCH_PTR(UpdateDescriptorSets);
	INIT_DISPATCH_PTR(CreateFramebuffer);
	INIT_DISPATCH_PTR(DestroyFramebuffer);
	INIT_DISPATCH_PTR(CreateRenderPass);
	INIT_DISPATCH_PTR(DestroyRenderPass);
	INIT_DISPATCH_PTR(CreateCommandPool);
	INIT_DISPATCH_PTR(DestroyCommandPool);
	INIT_DISPATCH_PTR(ResetCommandPool);
	INIT_DISPATCH_PTR(AllocateCommandBuffers);
	INIT_DISPATCH_PTR(FreeCommandBuffers);
	INIT_DISPATCH_PTR(BeginCommandBuffer);
	INIT_DISPATCH_PTR(EndCommandBuffer);
	INIT_DISPATCH_PTR(ResetCommandBuffer);
	INIT_DISPATCH_PTR(CmdBindPipeline);
	INIT_DISPATCH_PTR(CmdSetViewport);
	INIT_DISPATCH_PTR(CmdSetScissor);
	INIT_DISPATCH_PTR(CmdBindDescriptorSets);
	INIT_DISPATCH_PTR(CmdBindIndexBuffer);
	INIT_DISPATCH_PTR(CmdBindVertexBuffers);
	INIT_DISPATCH_PTR(CmdDraw);
	INIT_DISPATCH_PTR(CmdDrawIndexed);
	INIT_DISPATCH_PTR(CmdDrawIndirect);
	INIT_DISPATCH_PTR(CmdDrawIndexedIndirect);
	INIT_DISPATCH_PTR(CmdDispatch);
	INIT_DISPATCH_PTR(CmdDispatchIndirect);
	INIT_DISPATCH_PTR(CmdCopyBuffer);
	INIT_DISPATCH_PTR(CmdCopyImage);
	INIT_DISPATCH_PTR(CmdBlitImage);
	INIT_DISPATCH_PTR(CmdCopyBufferToImage);
	INIT_DISPATCH_PTR(CmdCopyImageToBuffer);
	INIT_DISPATCH_PTR(CmdUpdateBuffer);
	INIT_DISPATCH_PTR(CmdClearColorImage);
	INIT_DISPATCH_PTR(CmdClearDepthStencilImage);
	INIT_DISPATCH_PTR(CmdClearAttachments);
	INIT_DISPATCH_PTR(CmdResolveImage);
	INIT_DISPATCH_PTR(CmdPipelineBarrier);
	INIT_DISPATCH_PTR(CmdResetQueryPool);
	INIT_DISPATCH_PTR(CmdWriteTimestamp);
	INIT_DISPATCH_PTR(CmdPushConstants);
	INIT_DISPATCH_PTR(CmdBeginRenderPass);
	INIT_DISPATCH_PTR(CmdNextSubpass);
	INIT_DISPATCH_PTR(CmdEndRenderPass);
	INIT_DISPATCH_PTR(CmdExecuteCommands);
	// ---- Core 1_1 commands
	INIT_DISPATCH_PTR(BindBufferMemory2);
	INIT_DISPATCH_PTR(BindImageMemory2);
	INIT_DISPATCH_PTR(GetBufferMemoryRequirements2);
	INIT_DISPATCH_PTR(GetImageMemoryRequirements2);
	INIT_DISPATCH_PTR(GetDeviceQueue2);
	// ---- Core 1_2 commands
	INIT_DISPATCH_PTR(CreateRenderPass2);
	if (dispatch_table.CreateRenderPass2 == nullptr) // Try the KHR version if the core version does not exist
		dispatch_table.CreateRenderPass2  = reinterpret_cast<PFN_vkCreateRenderPass2KHR>(get_device_proc(device, "vkCreateRenderPass2KHR"));
	// ---- VK_KHR_swapchain extension commands
	INIT_DISPATCH_PTR(CreateSwapchainKHR);
	INIT_DISPATCH_PTR(DestroySwapchainKHR);
	INIT_DISPATCH_PTR(GetSwapchainImagesKHR);
	INIT_DISPATCH_PTR(QueuePresentKHR);
	// ---- VK_KHR_push_descriptor extension commands
	INIT_DISPATCH_PTR(CmdPushDescriptorSetKHR);
	// ---- VK_EXT_debug_marker extension commands
	INIT_DISPATCH_PTR(DebugMarkerSetObjectNameEXT);
	INIT_DISPATCH_PTR(CmdDebugMarkerBeginEXT);
	INIT_DISPATCH_PTR(CmdDebugMarkerEndEXT);

	// Initialize per-device data
	const auto device_impl = new reshade::vulkan::device_impl(
		device,
		physicalDevice,
		g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)),
		dispatch_table);

	device_impl->_graphics_queue_family_index = graphics_queue_family_index;

	g_vulkan_devices.emplace(dispatch_key_from_handle(device), device_impl);

	// Initialize all queues associated with this device
	for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
	{
		const VkDeviceQueueCreateInfo &queue_create_info = pCreateInfo->pQueueCreateInfos[i];

		for (uint32_t queue_index = 0; queue_index < queue_create_info.queueCount; ++queue_index)
		{
			VkQueue queue = VK_NULL_HANDLE;
			dispatch_table.GetDeviceQueue(device, queue_create_info.queueFamilyIndex, queue_index, &queue);
			assert(VK_NULL_HANDLE != queue);

			const auto queue_impl = new reshade::vulkan::command_queue_impl(
				device_impl,
				queue_create_info.queueFamilyIndex,
				queue_families[queue_create_info.queueFamilyIndex],
				queue);

			s_vulkan_queues.emplace(queue, queue_impl);
		}
	}

#if RESHADE_VERBOSE_LOG
	LOG(INFO) << "Returning Vulkan device " << device << '.';
#endif
	return VK_SUCCESS;
}
void     VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	LOG(INFO) << "Redirecting " << "vkDestroyDevice" << '(' << "device = " << device << ", pAllocator = " << pAllocator << ')' << " ...";

	s_vulkan_command_buffers.clear(); // Reset all command buffer data

	// Remove from device dispatch table since this device is being destroyed
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.erase(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	// Destroy all queues associated with this device
	const std::vector<reshade::vulkan::command_queue_impl *> queues = device_impl->_queues;
	for (reshade::vulkan::command_queue_impl *queue_impl : queues)
	{
		s_vulkan_queues.erase(queue_impl->_orig);
		delete queue_impl; // This will remove the queue from the queue list of the device too (see 'command_queue_impl' destructor)
	}
	assert(device_impl->_queues.empty());

	// Get function pointer before data is destroyed next
	GET_DISPATCH_PTR_FROM(DestroyDevice, device_impl);

	// Finally destroy the device
	delete device_impl;

	trampoline(device, pAllocator);
}

VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	LOG(INFO) << "Redirecting " << "vkCreateSwapchainKHR" << '(' << "device = " << device << ", pCreateInfo = " << pCreateInfo << ", pAllocator = " << pAllocator << ", pSwapchain = " << pSwapchain << ')' << " ...";

	assert(pCreateInfo != nullptr && pSwapchain != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
	VkImageFormatListCreateInfoKHR format_list_info { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR };
	std::vector<VkFormat> format_list; std::vector<uint32_t> queue_family_list;

	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	if (device_impl->_graphics_queue_family_index != std::numeric_limits<uint32_t>::max())
	{
		// Add required usage flags to create info
		create_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		// Add required formats, so views with different formats can be created for the swap chain images
		format_list.push_back(make_format_srgb(create_info.imageFormat));
		format_list.push_back(make_format_normal(create_info.imageFormat));

		// Only have to make format mutable if they are actually different
		if (format_list[0] != format_list[1])
			create_info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

		// Patch the format list in the create info of the application
		if (const VkImageFormatListCreateInfoKHR *format_list_info2 = find_in_structure_chain<VkImageFormatListCreateInfoKHR>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR); format_list_info2 != nullptr)
		{
			format_list.insert(format_list.end(),
				format_list_info2->pViewFormats, format_list_info2->pViewFormats + format_list_info2->viewFormatCount);

			// Remove duplicates from the list (since the new formats may have already been added by the application)
			std::sort(format_list.begin(), format_list.end());
			format_list.erase(std::unique(format_list.begin(), format_list.end()), format_list.end());

			// This is evil, because writing into the application memory, but eh =)
			const_cast<VkImageFormatListCreateInfoKHR *>(format_list_info2)->viewFormatCount = static_cast<uint32_t>(format_list.size());
			const_cast<VkImageFormatListCreateInfoKHR *>(format_list_info2)->pViewFormats = format_list.data();
		}
		else if (format_list[0] != format_list[1])
		{
			format_list_info.pNext = create_info.pNext;
			format_list_info.viewFormatCount = static_cast<uint32_t>(format_list.size());
			format_list_info.pViewFormats = format_list.data();

			create_info.pNext = &format_list_info;
		}

		// Add required queue family indices, so images can be used on the graphics queue
		if (create_info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
		{
			queue_family_list.reserve(create_info.queueFamilyIndexCount + 1);
			queue_family_list.push_back(device_impl->_graphics_queue_family_index);

			for (uint32_t i = 0; i < create_info.queueFamilyIndexCount; ++i)
				if (create_info.pQueueFamilyIndices[i] != device_impl->_graphics_queue_family_index)
					queue_family_list.push_back(create_info.pQueueFamilyIndices[i]);

			create_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_list.size());
			create_info.pQueueFamilyIndices = queue_family_list.data();
		}
	}

	LOG(INFO) << "> Dumping swap chain description:";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";
	LOG(INFO) << "  | Parameter                               | Value                                   |";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";
	LOG(INFO) << "  | flags                                   | " << std::setw(39) << std::hex << create_info.flags << std::dec << " |";
	LOG(INFO) << "  | surface                                 | " << std::setw(39) << create_info.surface << " |";
	LOG(INFO) << "  | minImageCount                           | " << std::setw(39) << create_info.minImageCount << " |";
	if (const char *format_string = format_to_string(create_info.imageFormat); format_string != nullptr)
		LOG(INFO) << "  | imageFormat                             | " << std::setw(39) << format_string << " |";
	else
		LOG(INFO) << "  | imageFormat                             | " << std::setw(39) << create_info.imageFormat << " |";
	LOG(INFO) << "  | imageColorSpace                         | " << std::setw(39) << create_info.imageColorSpace << " |";
	LOG(INFO) << "  | imageExtent                             | " << std::setw(19) << create_info.imageExtent.width << ' ' << std::setw(19) << create_info.imageExtent.height << " |";
	LOG(INFO) << "  | imageArrayLayers                        | " << std::setw(39) << create_info.imageArrayLayers << " |";
	LOG(INFO) << "  | imageUsage                              | " << std::setw(39) << std::hex << create_info.imageUsage << std::dec << " |";
	LOG(INFO) << "  | imageSharingMode                        | " << std::setw(39) << create_info.imageSharingMode << " |";
	LOG(INFO) << "  | queueFamilyIndexCount                   | " << std::setw(39) << create_info.queueFamilyIndexCount << " |";
	LOG(INFO) << "  | preTransform                            | " << std::setw(39) << std::hex << create_info.preTransform << std::dec << " |";
	LOG(INFO) << "  | compositeAlpha                          | " << std::setw(39) << std::hex << create_info.compositeAlpha << std::dec << " |";
	LOG(INFO) << "  | presentMode                             | " << std::setw(39) << create_info.presentMode << " |";
	LOG(INFO) << "  | clipped                                 | " << std::setw(39) << (create_info.clipped ? "true" : "false") << " |";
	LOG(INFO) << "  | oldSwapchain                            | " << std::setw(39) << create_info.oldSwapchain << " |";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";

	GET_DISPATCH_PTR_FROM(CreateSwapchainKHR, device_impl);
	const VkResult result = trampoline(device, &create_info, pAllocator, pSwapchain);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateSwapchainKHR" << " failed with error code " << result << '.';
		return result;
	}

#if RESHADE_ADDON
	// Add swap chain images to the image list
	uint32_t num_swapchain_images = 0;
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, *pSwapchain, &num_swapchain_images, nullptr);
	std::vector<VkImage> swapchain_images(num_swapchain_images);
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, *pSwapchain, &num_swapchain_images, swapchain_images.data());

	VkImageCreateInfo image_create_info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = create_info.imageFormat;
	image_create_info.extent = { create_info.imageExtent.width, create_info.imageExtent.height, 1 };
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = create_info.imageArrayLayers;
	image_create_info.usage = create_info.imageUsage;
	image_create_info.sharingMode = create_info.imageSharingMode;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	for (uint32_t i = 0; i < num_swapchain_images; ++i)
		device_impl->register_image(swapchain_images[i], image_create_info);
#endif

	reshade::vulkan::command_queue_impl *queue_impl = nullptr;
	if (device_impl->_graphics_queue_family_index != std::numeric_limits<uint32_t>::max())
	{
		// Get the main graphics queue for command submission
		// There has to be at least one queue, or else this runtime would not have been created with this queue family index, so it is safe to get the first one here
		VkQueue graphics_queue = VK_NULL_HANDLE;
		device_impl->_dispatch_table.GetDeviceQueue(device, device_impl->_graphics_queue_family_index, 0, &graphics_queue);
		assert(VK_NULL_HANDLE != graphics_queue);

		queue_impl = s_vulkan_queues.at(graphics_queue);
	}

	if (queue_impl != nullptr)
	{
		// Remove old swap chain from the list so that a call to 'vkDestroySwapchainKHR' won't reset the runtime again
		reshade::vulkan::runtime_impl *runtime = s_vulkan_runtimes.erase(create_info.oldSwapchain);
		if (runtime != nullptr)
		{
			assert(create_info.oldSwapchain != VK_NULL_HANDLE);

			reshade::invoke_addon_event_without_trampoline<reshade::addon_event::resize>(
				runtime, create_info.imageExtent.width, create_info.imageExtent.height);

			// Re-use the existing runtime if this swap chain was not created from scratch, but reset it before initializing again below
			runtime->on_reset();
		}
		else
		{
			runtime = new reshade::vulkan::runtime_impl(device_impl, queue_impl);
		}

		// Look up window handle from surface
		const HWND hwnd = g_surface_windows.at(create_info.surface);

		if (!runtime->on_init(*pSwapchain, create_info, hwnd))
			LOG(ERROR) << "Failed to initialize Vulkan runtime environment on runtime " << runtime << '.';

		if (!s_vulkan_runtimes.emplace(*pSwapchain, runtime))
			delete runtime;
	}
	else
	{
		s_vulkan_runtimes.emplace(*pSwapchain, nullptr);
	}

#if RESHADE_VERBOSE_LOG
	LOG(INFO) << "Returning Vulkan swapchain " << *pSwapchain << '.';
#endif
	return VK_SUCCESS;
}
void     VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	LOG(INFO) << "Redirecting " << "vkDestroySwapchainKHR" << '(' << device << ", " << swapchain << ", " << pAllocator << ')' << " ...";

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	// Remove runtime from global list
	delete s_vulkan_runtimes.erase(swapchain);

#if RESHADE_ADDON
	// Remove swap chain images from the image list
	uint32_t num_swapchain_images = 0;
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain, &num_swapchain_images, nullptr);
	std::vector<VkImage> swapchain_images(num_swapchain_images);
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain, &num_swapchain_images, swapchain_images.data());

	for (uint32_t i = 0; i < num_swapchain_images; ++i)
		device_impl->unregister_image(swapchain_images[i]);
#endif

	GET_DISPATCH_PTR_FROM(DestroySwapchainKHR, device_impl);
	trampoline(device, swapchain, pAllocator);
}

VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	assert(pSubmits != nullptr);

#if RESHADE_ADDON
	reshade::vulkan::command_queue_impl *const queue_impl = s_vulkan_queues.at(queue);
	if (queue_impl != nullptr)
	{
		queue_impl->flush_immediate_command_list();

		for (uint32_t i = 0; i < submitCount; ++i)
		{
			for (uint32_t k = 0; k < pSubmits[i].commandBufferCount; ++k)
			{
				assert(pSubmits[i].pCommandBuffers[k] != VK_NULL_HANDLE);

				reshade::invoke_addon_event_without_trampoline<reshade::addon_event::execute_command_list>(
					queue_impl, s_vulkan_command_buffers.at(pSubmits[i].pCommandBuffers[k]));
			}
		}
	}
#endif

	// The loader uses the same dispatch table pointer for queues and devices, so can use queue to perform lookup here
	GET_DISPATCH_PTR(QueueSubmit, queue);
	return trampoline(queue, submitCount, pSubmits, fence);
}
VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	assert(pPresentInfo != nullptr);

	std::vector<VkSemaphore> wait_semaphores(
		pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);

	reshade::vulkan::command_queue_impl *const queue_impl = s_vulkan_queues.at(queue);
	if (queue_impl != nullptr)
	{
		for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
		{
			if (const auto runtime = s_vulkan_runtimes.at(pPresentInfo->pSwapchains[i]);
				runtime != nullptr)
			{
				reshade::invoke_addon_event_without_trampoline<reshade::addon_event::present>(queue_impl, runtime);

				runtime->on_present(queue, pPresentInfo->pImageIndices[i], wait_semaphores);
			}
		}
	}

	// Override wait semaphores based on the last queue submit from above
	VkPresentInfoKHR present_info = *pPresentInfo;
	present_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
	present_info.pWaitSemaphores = wait_semaphores.data();

	GET_DISPATCH_PTR(QueuePresentKHR, queue);
	return trampoline(queue, &present_info);
}

VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
	assert(pCreateInfo != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);
	VkBufferCreateInfo create_info = *pCreateInfo;

	VkResult result = VK_ERROR_UNKNOWN;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[device_impl, &result, &create_info, pAllocator, pBuffer](reshade::api::device *,const reshade::api::resource_desc &desc, reshade::api::resource_usage initial_state, const reshade::api::mapped_subresource *initial_data) {
			if (desc.type != reshade::api::resource_type::buffer || desc.mem_usage != reshade::api::memory_usage::unknown || initial_state != reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::vulkan::convert_resource_desc(desc, create_info);

			GET_DISPATCH_PTR_FROM(CreateBuffer, device_impl);
			result = trampoline(device_impl->_orig, &create_info, pAllocator, pBuffer);
			if (result == VK_SUCCESS)
			{
				assert(pBuffer != nullptr);
				device_impl->register_buffer(*pBuffer, create_info);
				return true;
			}
			else
			{
				LOG(WARN) << "vkCreateBuffer" << " failed with error code " << result << '.';
				return false;
			}
		}, device_impl, reshade::vulkan::convert_resource_desc(create_info), reshade::api::resource_usage::undefined, nullptr);
	return result;
}
void     VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->unregister_buffer(buffer);
#endif

	GET_DISPATCH_PTR_FROM(DestroyBuffer, device_impl);
	trampoline(device, buffer, pAllocator);
}

VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
	assert(pCreateInfo != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);
	VkBufferViewCreateInfo create_info = *pCreateInfo;

	VkResult result = VK_ERROR_UNKNOWN;
	reshade::invoke_addon_event<reshade::addon_event::create_resource_view>(
		[device_impl, &result, &create_info, pAllocator, pView](reshade::api::device *, reshade::api::resource_handle resource, reshade::api::resource_usage, const reshade::api::resource_view_desc &desc) {
			if (desc.type != reshade::api::resource_view_type::buffer)
				return false;
			create_info.buffer = (VkBuffer)resource.handle;
			reshade::vulkan::convert_resource_view_desc(desc, create_info);

			GET_DISPATCH_PTR_FROM(CreateBufferView, device_impl);
			result = trampoline(device_impl->_orig, &create_info, pAllocator, pView);
			if (result == VK_SUCCESS)
			{
				assert(pView != nullptr);
				device_impl->register_buffer_view(*pView, create_info);
				return true;
			}
			else
			{
				LOG(WARN) << "vkCreateBufferView" << " failed with error code " << result << '.';
				return false;
			}
		}, device_impl, reshade::api::resource_handle { (uint64_t)create_info.buffer }, reshade::api::resource_usage::undefined, reshade::vulkan::convert_resource_view_desc(create_info));
	return result;
}
void     VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->unregister_buffer_view(bufferView);
#endif

	GET_DISPATCH_PTR_FROM(DestroyBufferView, device_impl);
	trampoline(device, bufferView, pAllocator);
}

VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
	assert(pCreateInfo != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);
	VkImageCreateInfo create_info = *pCreateInfo;

	VkResult result = VK_ERROR_UNKNOWN;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[device_impl, &result, &create_info, pAllocator, pImage](reshade::api::device *, const reshade::api::resource_desc &desc, reshade::api::resource_usage initial_state, const reshade::api::mapped_subresource *initial_data) {
			if (desc.type == reshade::api::resource_type::buffer || desc.mem_usage != reshade::api::memory_usage::unknown || initial_state != reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::vulkan::convert_resource_desc(desc, create_info);

			GET_DISPATCH_PTR_FROM(CreateImage, device_impl);
			result = trampoline(device_impl->_orig, &create_info, pAllocator, pImage);
			if (result == VK_SUCCESS)
			{
				assert(pImage != nullptr);
				device_impl->register_image(*pImage, create_info);
				return true;
			}
			else
			{
				LOG(WARN) << "vkCreateImage" << " failed with error code " << result << '.';
				return false;
			}
		}, device_impl, reshade::vulkan::convert_resource_desc(create_info), reshade::api::resource_usage::undefined, nullptr);
	return result;
}
void     VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->unregister_image(image);
#endif

	GET_DISPATCH_PTR_FROM(DestroyImage, device_impl);
	trampoline(device, image, pAllocator);
}

VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	assert(pCreateInfo != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);
	VkImageViewCreateInfo create_info = *pCreateInfo;

	VkResult result = VK_ERROR_UNKNOWN;
	reshade::invoke_addon_event<reshade::addon_event::create_resource_view>(
		[device_impl, &result, &create_info, pAllocator, pView](reshade::api::device *, reshade::api::resource_handle resource, reshade::api::resource_usage, const reshade::api::resource_view_desc &desc) {
			if (desc.type == reshade::api::resource_view_type::buffer)
				return false;
			create_info.image = (VkImage)resource.handle;
			reshade::vulkan::convert_resource_view_desc(desc, create_info);

			GET_DISPATCH_PTR_FROM(CreateImageView, device_impl);
			result = trampoline(device_impl->_orig, &create_info, pAllocator, pView);
			if (result == VK_SUCCESS)
			{
				assert(pView != nullptr);
				device_impl->register_image_view(*pView, create_info);
				return true;
			}
			else
			{
				LOG(WARN) << "vkCreateImageView" << " failed with error code " << result << '.';
				return false;
			}
		}, device_impl, reshade::api::resource_handle { (uint64_t)create_info.image }, reshade::api::resource_usage::undefined, reshade::vulkan::convert_resource_view_desc(create_info));
	return result;
}
void     VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->unregister_image_view(imageView);
#endif

	GET_DISPATCH_PTR_FROM(DestroyImageView, device_impl);
	trampoline(device, imageView, pAllocator);
}

VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
	assert(pCreateInfo != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);
	VkShaderModuleCreateInfo create_info = *pCreateInfo;

	VkResult result = VK_ERROR_UNKNOWN;
	reshade::invoke_addon_event<reshade::addon_event::create_shader_module>(
		[device_impl, &result, &create_info, pAllocator, pShaderModule](reshade::api::device *, const void *code, size_t code_size) {
			create_info.pCode = static_cast<const uint32_t *>(code);
			create_info.codeSize = code_size;

			GET_DISPATCH_PTR_FROM(CreateShaderModule, device_impl);
			result = trampoline(device_impl->_orig, &create_info, pAllocator, pShaderModule);
			if (result == VK_SUCCESS)
			{
				return true;
			}
			else
			{
				LOG(WARN) << "vkCreateShaderModule" << " failed with error code " << result << '.';
				return false;
			}
		}, device_impl, create_info.pCode, create_info.codeSize);

	return result;
}

VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	assert(pCreateInfo != nullptr && pRenderPass != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	GET_DISPATCH_PTR_FROM(CreateRenderPass, device_impl);
	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pRenderPass);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateRenderPass" << " failed with error code " << result << '.';
		return result;
	}

#if RESHADE_ADDON
	auto &renderpass_data = device_impl->_render_pass_list.emplace(*pRenderPass);
	renderpass_data.subpasses.reserve(pCreateInfo->subpassCount);
	renderpass_data.cleared_attachments.reserve(pCreateInfo->attachmentCount);

	for (uint32_t subpass = 0; subpass < pCreateInfo->subpassCount; ++subpass)
	{
		auto &subpass_data = renderpass_data.subpasses.emplace_back();
		subpass_data.color_attachments.resize(pCreateInfo->pSubpasses[subpass].colorAttachmentCount);

		for (uint32_t i = 0; i < pCreateInfo->pSubpasses[subpass].colorAttachmentCount; ++i)
		{
			const VkAttachmentReference *const reference = pCreateInfo->pSubpasses[subpass].pColorAttachments + i;
			if (reference->attachment != VK_ATTACHMENT_UNUSED)
				subpass_data.color_attachments[i] = *reference;
			else
				subpass_data.color_attachments[i] = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
		}

		{
			const VkAttachmentReference *const reference = pCreateInfo->pSubpasses[subpass].pDepthStencilAttachment;
			if (reference != nullptr && reference->attachment != VK_ATTACHMENT_UNUSED)
				subpass_data.depth_stencil_attachment = *reference;
			else
				subpass_data.depth_stencil_attachment = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
		}
	}

	for (uint32_t attachment = 0; attachment < pCreateInfo->attachmentCount; ++attachment)
	{
		const uint32_t clear_flags =
			(pCreateInfo->pAttachments[attachment].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 0x1 : 0x0) |
			(pCreateInfo->pAttachments[attachment].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 0x2 : 0x0);

		if (clear_flags != 0)
			renderpass_data.cleared_attachments.push_back({ clear_flags, attachment, pCreateInfo->pAttachments[attachment].initialLayout });
	}
#endif

	return VK_SUCCESS;
}
VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	assert(pCreateInfo != nullptr && pRenderPass != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	GET_DISPATCH_PTR_FROM(CreateRenderPass2, device_impl);
	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pRenderPass);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateRenderPass2" << " failed with error code " << result << '.';
		return result;
	}

#if RESHADE_ADDON
	auto &renderpass_data = device_impl->_render_pass_list.emplace(*pRenderPass);
	renderpass_data.subpasses.reserve(pCreateInfo->subpassCount);
	renderpass_data.cleared_attachments.reserve(pCreateInfo->attachmentCount);

	for (uint32_t subpass = 0; subpass < pCreateInfo->subpassCount; ++subpass)
	{
		auto &subpass_data = renderpass_data.subpasses.emplace_back();
		subpass_data.color_attachments.resize(pCreateInfo->pSubpasses[subpass].colorAttachmentCount);

		for (uint32_t i = 0; i < pCreateInfo->pSubpasses[subpass].colorAttachmentCount; ++i)
		{
			const VkAttachmentReference2 *const reference = pCreateInfo->pSubpasses[subpass].pColorAttachments + i;
			if (reference->attachment != VK_ATTACHMENT_UNUSED)
				subpass_data.color_attachments[i] = { reference->attachment, reference->layout };
			else
				subpass_data.color_attachments[i] = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
		}

		{
			const VkAttachmentReference2 *const reference = pCreateInfo->pSubpasses[subpass].pDepthStencilAttachment;
			if (reference != nullptr && reference->attachment != VK_ATTACHMENT_UNUSED)
				subpass_data.depth_stencil_attachment = { reference->attachment, reference->layout };
			else
				subpass_data.depth_stencil_attachment = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
		}
	}

	for (uint32_t attachment = 0; attachment < pCreateInfo->attachmentCount; ++attachment)
	{
		const uint32_t clear_flags =
			(pCreateInfo->pAttachments[attachment].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 0x1 : 0x0) |
			(pCreateInfo->pAttachments[attachment].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 0x2 : 0x0);

		if (clear_flags != 0)
			renderpass_data.cleared_attachments.push_back({ clear_flags, attachment, pCreateInfo->pAttachments[attachment].initialLayout });
	}
#endif

	return VK_SUCCESS;
}
void     VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->_render_pass_list.erase(renderPass);
#endif

	GET_DISPATCH_PTR_FROM(DestroyRenderPass, device_impl);
	trampoline(device, renderPass, pAllocator);
}

VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	GET_DISPATCH_PTR_FROM(CreateFramebuffer, device_impl);
	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pFramebuffer);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateFramebuffer" << " failed with error code " << result << '.';
		return result;
	}

#if RESHADE_ADDON
	// Keep track of the frame buffer attachments
	auto &attachments = device_impl->_framebuffer_list.emplace(*pFramebuffer);
	attachments.resize(pCreateInfo->attachmentCount);
	for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
		attachments[i] = { (uint64_t)pCreateInfo->pAttachments[i] };
#endif

	return VK_SUCCESS;
}
void     VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	device_impl->_framebuffer_list.erase(framebuffer);
#endif

	GET_DISPATCH_PTR_FROM(DestroyFramebuffer, device_impl);
	trampoline(device, framebuffer, pAllocator);
}

VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	GET_DISPATCH_PTR_FROM(AllocateCommandBuffers, device_impl);
	const VkResult result = trampoline(device, pAllocateInfo, pCommandBuffers);
	if (result != VK_SUCCESS)
	{
		LOG(WARN) << "vkAllocateCommandBuffers" << " failed with error code " << result << '.';
		return result;
	}

#if RESHADE_ADDON
	for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
	{
		const auto cmd_impl = new reshade::vulkan::command_list_impl(device_impl, pCommandBuffers[i]);
		if (!s_vulkan_command_buffers.emplace(pCommandBuffers[i], cmd_impl))
			delete cmd_impl;
	}
#endif

	return VK_SUCCESS;
}
void     VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
#if RESHADE_ADDON
	for (uint32_t i = 0; i < commandBufferCount; ++i)
	{
		delete s_vulkan_command_buffers.erase(pCommandBuffers[i]);
	}
#endif

	GET_DISPATCH_PTR(FreeCommandBuffers, device);
	trampoline(device, commandPool, commandBufferCount, pCommandBuffers);
}

VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
	// Begin does perform an implicit reset if command pool was created with 'VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT'
	reshade::invoke_addon_event_without_trampoline<reshade::addon_event::reset_command_list>(
		s_vulkan_command_buffers.at(commandBuffer));

	GET_DISPATCH_PTR(BeginCommandBuffer, commandBuffer);
	return trampoline(commandBuffer, pBeginInfo);
}

void     VKAPI_CALL vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport *pViewports)
{
	static_assert(sizeof(VkViewport) == (sizeof(float) * 6));

	reshade::invoke_addon_event<reshade::addon_event::set_viewports>(
		[commandBuffer](reshade::api::command_list *, uint32_t first, uint32_t count, const float *viewports) {
			GET_DISPATCH_PTR(CmdSetViewport, commandBuffer);
			trampoline(commandBuffer, first, count, reinterpret_cast<const VkViewport *>(viewports));
		}, s_vulkan_command_buffers.at(commandBuffer), firstViewport, viewportCount, reinterpret_cast<const float *>(pViewports));
}
void     VKAPI_CALL vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D *pScissors)
{
	static_assert(sizeof(VkRect2D) == (sizeof(int32_t) * 4));

	reshade::invoke_addon_event<reshade::addon_event::set_scissor_rects>(
		[commandBuffer](reshade::api::command_list *, uint32_t first, uint32_t count, const int32_t *rects) {
			GET_DISPATCH_PTR(CmdSetScissor, commandBuffer);
			trampoline(commandBuffer, first, count, reinterpret_cast<const VkRect2D *>(rects));
		}, s_vulkan_command_buffers.at(commandBuffer), firstScissor, scissorCount, reinterpret_cast<const int32_t *>(pScissors));
}

void     VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
	reshade::invoke_addon_event<reshade::addon_event::set_index_buffer>(
		[commandBuffer](reshade::api::command_list *, reshade::api::resource_handle buffer, uint32_t format, uint64_t offset) {
			GET_DISPATCH_PTR(CmdBindIndexBuffer, commandBuffer);
			trampoline(commandBuffer, (VkBuffer)buffer.handle, offset, static_cast<VkIndexType>(format));
		}, s_vulkan_command_buffers.at(commandBuffer), reshade::api::resource_handle { (uint64_t)buffer }, static_cast<uint32_t>(indexType), offset);
}
void     VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets)
{
	static_assert(sizeof(VkBuffer) == sizeof(reshade::api::resource_handle));

	reshade::invoke_addon_event<reshade::addon_event::set_vertex_buffers>(
		[commandBuffer](reshade::api::command_list *, uint32_t first, uint32_t count, const reshade::api::resource_handle *buffers, const uint32_t *, const uint64_t *offsets) {
			GET_DISPATCH_PTR(CmdBindVertexBuffers, commandBuffer);
			trampoline(commandBuffer, first, count, reinterpret_cast<const VkBuffer *>(buffers), offsets);
		}, s_vulkan_command_buffers.at(commandBuffer), firstBinding, bindingCount, reinterpret_cast<const reshade::api::resource_handle *>(pBuffers), nullptr, pOffsets);
}

void     VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
	reshade::invoke_addon_event<reshade::addon_event::draw>(
		[commandBuffer](reshade::api::command_list *, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
			GET_DISPATCH_PTR(CmdDraw, commandBuffer);
			trampoline(commandBuffer, vertices, instances, first_vertex, first_instance);
		}, s_vulkan_command_buffers.at(commandBuffer), vertexCount, instanceCount, firstVertex, firstInstance);
}
void     VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	reshade::invoke_addon_event<reshade::addon_event::draw_indexed>(
		[commandBuffer](reshade::api::command_list *, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
			GET_DISPATCH_PTR(CmdDrawIndexed, commandBuffer);
			trampoline(commandBuffer, indices, instances, first_index, vertex_offset, first_instance);
		}, s_vulkan_command_buffers.at(commandBuffer), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
void     VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
	reshade::invoke_addon_event<reshade::addon_event::draw_or_dispatch_indirect>(
		[commandBuffer](reshade::api::command_list *, reshade::addon_event type, reshade::api::resource_handle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
			assert(type == reshade::addon_event::draw);
			GET_DISPATCH_PTR(CmdDrawIndirect, commandBuffer);
			trampoline(commandBuffer, (VkBuffer)buffer.handle, offset, draw_count, stride);
		}, s_vulkan_command_buffers.at(commandBuffer), reshade::addon_event::draw, reshade::api::resource_handle { (uint64_t)buffer }, offset, drawCount, stride);
}
void     VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
	reshade::invoke_addon_event<reshade::addon_event::draw_or_dispatch_indirect>(
		[commandBuffer](reshade::api::command_list *, reshade::addon_event type, reshade::api::resource_handle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
			assert(type == reshade::addon_event::draw_indexed);
			GET_DISPATCH_PTR(CmdDrawIndexedIndirect, commandBuffer);
			trampoline(commandBuffer, (VkBuffer)buffer.handle, offset, draw_count, stride);
		}, s_vulkan_command_buffers.at(commandBuffer), reshade::addon_event::draw_indexed, reshade::api::resource_handle { (uint64_t)buffer }, offset, drawCount, stride);
}
void     VKAPI_CALL vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	reshade::invoke_addon_event<reshade::addon_event::dispatch>(
		[commandBuffer](reshade::api::command_list *, uint32_t num_groups_x, uint32_t num_groups_y, uint32_t num_groups_z) {
			GET_DISPATCH_PTR(CmdDispatch, commandBuffer);
			trampoline(commandBuffer, num_groups_x, num_groups_y, num_groups_z);
		}, s_vulkan_command_buffers.at(commandBuffer), groupCountX, groupCountY, groupCountZ);
}
void     VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset)
{
	reshade::invoke_addon_event<reshade::addon_event::draw_or_dispatch_indirect>(
		[commandBuffer](reshade::api::command_list *, reshade::addon_event type, reshade::api::resource_handle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
			assert(type == reshade::addon_event::dispatch && draw_count == 1 && stride == 0);
			GET_DISPATCH_PTR(CmdDispatchIndirect, commandBuffer);
			trampoline(commandBuffer, (VkBuffer)buffer.handle, offset);
		}, s_vulkan_command_buffers.at(commandBuffer), reshade::addon_event::dispatch, reshade::api::resource_handle { (uint64_t)buffer }, offset, 1, 0);
}

void     VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(commandBuffer));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	VkImageMemoryBarrier transition { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	transition.oldLayout = imageLayout;
	transition.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.image = image;

	for (uint32_t i = 0; i < rangeCount; ++i)
	{
		transition.subresourceRange.aspectMask |= pRanges[i].aspectMask;
		transition.subresourceRange.baseMipLevel = std::min(transition.subresourceRange.baseMipLevel, pRanges[i].baseMipLevel);
		transition.subresourceRange.levelCount = std::max(transition.subresourceRange.levelCount, pRanges[i].levelCount);
		transition.subresourceRange.baseArrayLayer = std::min(transition.subresourceRange.baseArrayLayer, pRanges[i].baseArrayLayer);
		transition.subresourceRange.layerCount = std::max(transition.subresourceRange.layerCount, pRanges[i].layerCount);
	}

	device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

	reshade::invoke_addon_event<reshade::addon_event::clear_render_target>(
		[device_impl, &transition, commandBuffer, imageLayout, rangeCount, pRanges](reshade::api::command_list *, reshade::api::resource_view_handle rtv, const float color[4]) {
			reshade::api::resource_handle image = { 0 };
			device_impl->get_resource_from_view(rtv, &image);

			std::swap(transition.oldLayout, transition.newLayout);
			device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

			static_assert(sizeof(VkClearColorValue) == (sizeof(float) * 4));

			GET_DISPATCH_PTR_FROM(CmdClearColorImage, device_impl);
			trampoline(commandBuffer, (VkImage)image.handle, imageLayout, reinterpret_cast<const VkClearColorValue *>(color), rangeCount, pRanges);
		}, s_vulkan_command_buffers.at(commandBuffer), device_impl->get_default_view(image), pColor->float32);
#else
	GET_DISPATCH_PTR_FROM(CmdClearColorImage, device_impl);
	trampoline(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
#endif
}
void     VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(commandBuffer));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	VkImageMemoryBarrier transition { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	transition.oldLayout = imageLayout;
	transition.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.image = image;

	for (uint32_t i = 0; i < rangeCount; ++i)
	{
		transition.subresourceRange.aspectMask |= pRanges[i].aspectMask;
		transition.subresourceRange.baseMipLevel = std::min(transition.subresourceRange.baseMipLevel, pRanges[i].baseMipLevel);
		transition.subresourceRange.levelCount = std::max(transition.subresourceRange.levelCount, pRanges[i].levelCount);
		transition.subresourceRange.baseArrayLayer = std::min(transition.subresourceRange.baseArrayLayer, pRanges[i].baseArrayLayer);
		transition.subresourceRange.layerCount = std::max(transition.subresourceRange.layerCount, pRanges[i].layerCount);
	}

	device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

	const uint32_t clear_flags =
		(transition.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ? 0x1 : 0x0) |
		(transition.subresourceRange.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ? 0x2 : 0x0);

	reshade::invoke_addon_event<reshade::addon_event::clear_depth_stencil>(
		[device_impl, &transition, commandBuffer, imageLayout, rangeCount, pRanges](reshade::api::command_list *, reshade::api::resource_view_handle dsv, uint32_t clear_flags, float depth, uint8_t stencil) {
			reshade::api::resource_handle image = { 0 };
			device_impl->get_resource_from_view(dsv, &image);

			std::swap(transition.oldLayout, transition.newLayout);
			device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

			const VkClearDepthStencilValue clear_value = { depth, stencil };
			GET_DISPATCH_PTR_FROM(CmdClearDepthStencilImage, device_impl);
			trampoline(commandBuffer, (VkImage)image.handle, imageLayout, &clear_value, rangeCount, pRanges);
		}, s_vulkan_command_buffers.at(commandBuffer), device_impl->get_default_view(image), clear_flags, pDepthStencil->depth, static_cast<uint8_t>(pDepthStencil->stencil));
#else
	GET_DISPATCH_PTR_FROM(CmdClearDepthStencilImage, device_impl);
	trampoline(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
#endif
}

void     VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin, VkSubpassContents contents)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(commandBuffer));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	reshade::vulkan::command_list_impl *const cmd_impl = s_vulkan_command_buffers.at(commandBuffer);
	if (cmd_impl != nullptr)
	{
		cmd_impl->current_subpass = 0;
		cmd_impl->current_renderpass = pRenderPassBegin->renderPass;
		cmd_impl->current_framebuffer = pRenderPassBegin->framebuffer;

		const auto &attachments = device_impl->_framebuffer_list.at(cmd_impl->current_framebuffer);
		const auto &renderpass_data = device_impl->_render_pass_list.at(cmd_impl->current_renderpass);
		const auto &renderpass_data_subpass = renderpass_data.subpasses[0];

		auto rtvs = static_cast<reshade::api::resource_view_handle *>(alloca(sizeof(reshade::api::resource_view_handle) * renderpass_data_subpass.color_attachments.size()));
		for (uint32_t i = 0; i < renderpass_data_subpass.color_attachments.size(); ++i)
			if (renderpass_data_subpass.color_attachments[i].attachment != VK_ATTACHMENT_UNUSED)
				rtvs[i] = attachments[renderpass_data_subpass.color_attachments[i].attachment];
			else
				rtvs[i] = reshade::api::resource_view_handle { 0 };

		reshade::api::resource_view_handle dsv = { 0 };
		if (renderpass_data_subpass.depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED)
			dsv = attachments[renderpass_data_subpass.depth_stencil_attachment.attachment];

		reshade::invoke_addon_event<reshade::addon_event::set_render_targets_and_depth_stencil>(
			[](reshade::api::command_list *, uint32_t count, const reshade::api::resource_view_handle *new_rtvs, reshade::api::resource_view_handle new_dsv) {
				// TODO
			}, cmd_impl, static_cast<uint32_t>(renderpass_data_subpass.color_attachments.size()), rtvs, dsv);

		assert(renderpass_data.cleared_attachments.size() == pRenderPassBegin->clearValueCount);
		for (uint32_t i = 0; i < renderpass_data.cleared_attachments.size() && i < pRenderPassBegin->clearValueCount; ++i)
		{
			const VkClearValue &clear_value = pRenderPassBegin->pClearValues[i];

			if (renderpass_data.cleared_attachments[i].index != renderpass_data_subpass.depth_stencil_attachment.attachment)
			{
				if (!reshade::addon::event_list[static_cast<size_t>(reshade::addon_event::clear_render_target)].empty())
				{
					reshade::api::resource_handle image = { 0 };
					device_impl->get_resource_from_view(attachments[renderpass_data.cleared_attachments[i].index], &image);

					VkImageMemoryBarrier transition { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
					transition.oldLayout = renderpass_data.cleared_attachments[i].initial_layout;
					transition.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					transition.image = (VkImage)image.handle;
					transition.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

					assert(renderpass_data.cleared_attachments[i].clear_flags == 0x1);

					device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

					reshade::invoke_addon_event<reshade::addon_event::clear_render_target>(
						[](reshade::api::command_list *, reshade::api::resource_view_handle rtv, const float color[4]) {
							// TODO
						}, cmd_impl, attachments[renderpass_data.cleared_attachments[i].index], clear_value.color.float32);

					std::swap(transition.oldLayout, transition.newLayout);
					device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
				}
			}
			else
			{
				if (!reshade::addon::event_list[static_cast<size_t>(reshade::addon_event::clear_depth_stencil)].empty())
				{
					reshade::api::resource_handle image = { 0 };
					device_impl->get_resource_from_view(attachments[renderpass_data.cleared_attachments[i].index], &image);

					VkImageMemoryBarrier transition { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
					transition.oldLayout = renderpass_data.cleared_attachments[i].initial_layout;
					transition.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					transition.image = (VkImage)image.handle;
					transition.subresourceRange = { 0, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

					if ((renderpass_data.cleared_attachments[i].clear_flags & 0x1) == 0x1)
						transition.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
					if ((renderpass_data.cleared_attachments[i].clear_flags & 0x2) == 0x2)
						transition.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

					device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);

					reshade::invoke_addon_event<reshade::addon_event::clear_depth_stencil>(
						[](reshade::api::command_list *, reshade::api::resource_view_handle dsv, uint32_t clear_flags, float depth, uint8_t stencil) {
							// TODO
						}, cmd_impl, attachments[renderpass_data.cleared_attachments[i].index], renderpass_data.cleared_attachments[i].clear_flags, clear_value.depthStencil.depth, static_cast<uint8_t>(clear_value.depthStencil.stencil));

					std::swap(transition.oldLayout, transition.newLayout);
					device_impl->_dispatch_table.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
				}
			}
		}
	}
#endif

	GET_DISPATCH_PTR_FROM(CmdBeginRenderPass, device_impl);
	trampoline(commandBuffer, pRenderPassBegin, contents);
}
void     VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(commandBuffer));
	assert(device_impl != nullptr);

#if RESHADE_ADDON
	reshade::vulkan::command_list_impl *const cmd_impl = s_vulkan_command_buffers.at(commandBuffer);
	if (cmd_impl != nullptr)
	{
		cmd_impl->current_subpass++;
		assert(cmd_impl->current_renderpass != VK_NULL_HANDLE);
		assert(cmd_impl->current_framebuffer != VK_NULL_HANDLE);

		const auto &attachments = device_impl->_framebuffer_list.at(cmd_impl->current_framebuffer);
		const auto &renderpass_data = device_impl->_render_pass_list.at(cmd_impl->current_renderpass);
		const auto &renderpass_data_subpass = renderpass_data.subpasses[cmd_impl->current_subpass];

		auto rtvs = static_cast<reshade::api::resource_view_handle *>(alloca(sizeof(reshade::api::resource_view_handle) * renderpass_data_subpass.color_attachments.size()));
		for (uint32_t i = 0; i < renderpass_data_subpass.color_attachments.size(); ++i)
			if (renderpass_data_subpass.color_attachments[i].attachment != VK_ATTACHMENT_UNUSED)
				rtvs[i] = attachments[renderpass_data_subpass.color_attachments[i].attachment];
			else
				rtvs[i] = reshade::api::resource_view_handle { 0 };

		reshade::api::resource_view_handle dsv = { 0 };
		if (renderpass_data_subpass.depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED)
			dsv = attachments[renderpass_data_subpass.depth_stencil_attachment.attachment];

		reshade::invoke_addon_event<reshade::addon_event::set_render_targets_and_depth_stencil>(
			[](reshade::api::command_list *, uint32_t count, const reshade::api::resource_view_handle *new_rtvs, reshade::api::resource_view_handle new_dsv) {
				// TODO
			}, cmd_impl, static_cast<uint32_t>(renderpass_data_subpass.color_attachments.size()), rtvs, dsv);
	}
#endif

	GET_DISPATCH_PTR_FROM(CmdNextSubpass, device_impl);
	trampoline(commandBuffer, contents);
}
void     VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
#if RESHADE_ADDON
	reshade::vulkan::command_list_impl *const cmd_impl = s_vulkan_command_buffers.at(commandBuffer);
	if (cmd_impl != nullptr)
	{
		cmd_impl->current_subpass = std::numeric_limits<uint32_t>::max();
		cmd_impl->current_renderpass = VK_NULL_HANDLE;
		cmd_impl->current_framebuffer = VK_NULL_HANDLE;
	}
#endif

	GET_DISPATCH_PTR(CmdEndRenderPass, commandBuffer);
	trampoline(commandBuffer);
}

void     VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
	for (uint32_t i = 0; i < commandBufferCount; ++i)
		reshade::invoke_addon_event_without_trampoline<reshade::addon_event::execute_secondary_command_list>(
			s_vulkan_command_buffers.at(commandBuffer),
			s_vulkan_command_buffers.at(pCommandBuffers[i]));

	GET_DISPATCH_PTR(CmdExecuteCommands, commandBuffer);
	trampoline(commandBuffer, commandBufferCount, pCommandBuffers);
}
