cordite_add_module(ui
    SOURCES
        ImGuiConfig.h
        UiFonts.cpp
        UiFonts.h
        UiIcons.h
        UiRenderer.cpp
        UiRenderer.h
        UiSystem.cpp
        UiSystem.h
        UiTheme.cpp
        UiTheme.h
        UiWidgets.cpp
        UiWidgets.h
    DEPENDS
        core
        math
        platform
        rhi
        shaders
        renderer
    PUBLIC_LIBRARIES
        cordite::imgui)

# The typefaces and the icons of the interface, compiled into the module (UiFonts.cpp).
cordite_embed_file(cordite_ui interRegular "${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf")
cordite_embed_file(cordite_ui interSemiBold "${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf")
cordite_embed_file(cordite_ui jetBrainsMonoRegular "${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMonoNL-Regular.ttf")
cordite_embed_file(cordite_ui lucide "${lucide_SOURCE_DIR}/font/lucide.ttf")
