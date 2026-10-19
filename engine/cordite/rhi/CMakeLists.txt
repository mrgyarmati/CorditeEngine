set(cordite_rhi_sources
    DeviceBackend.h
    GraphicsDevice.cpp
    GraphicsDevice.h
    Swapchain.cpp
    Swapchain.h
    TextureReadback.cpp
    TextureReadback.h
    vulkan/VulkanContext.h
    vulkan/VulkanDevice.cpp)
# Direct3D 12 is a graphics API of Windows alone.
if(WIN32)
    list(APPEND cordite_rhi_sources d3d12/D3D12Device.cpp)
endif()

cordite_add_module(rhi
    SOURCES
        ${cordite_rhi_sources}
    DEPENDS
        core
        math
        platform
    PUBLIC_LIBRARIES
        nvrhi
    PRIVATE_LIBRARIES
        nvrhi_vk
        Vulkan::Headers)

if(WIN32)
    # NVRHI's backend brings Direct3D 12 and the GUIDs of its interfaces; DXGI finds the
    # adapters.
    target_link_libraries(cordite_rhi PRIVATE nvrhi_d3d12 dxgi)
endif()

# NVRHI is built with the dynamic Vulkan-Hpp dispatcher and, as a static library, leaves
# its storage to the application. Our Vulkan code must use the same configuration, or the
# two halves of the program would disagree about how Vulkan functions are called.
target_compile_definitions(cordite_rhi PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
