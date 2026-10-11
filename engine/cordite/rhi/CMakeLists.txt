cordite_add_module(rhi
    SOURCES
        GraphicsDevice.cpp
        GraphicsDevice.h
        Swapchain.cpp
        Swapchain.h
        TextureReadback.cpp
        TextureReadback.h
        vulkan/VulkanContext.h
    DEPENDS
        core
        math
        platform
    PUBLIC_LIBRARIES
        nvrhi
    PRIVATE_LIBRARIES
        nvrhi_vk
        Vulkan::Headers)

# NVRHI is built with the dynamic Vulkan-Hpp dispatcher and, as a static library, leaves
# its storage to the application. Our Vulkan code must use the same configuration, or the
# two halves of the program would disagree about how Vulkan functions are called.
target_compile_definitions(cordite_rhi PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
