cordite_add_module(editor
    SOURCES
        AssetBrowser.cpp
        AssetBrowser.h
        AssetBrowserPanel.cpp
        AssetBrowserPanel.h
        AssetCommands.cpp
        AssetCommands.h
        AssetDrop.cpp
        AssetDrop.h
        AssetImport.cpp
        AssetImport.h
        AssetMove.cpp
        AssetMove.h
        AssetPreview.cpp
        AssetPreview.h
        ColliderOutlines.cpp
        ColliderOutlines.h
        StatisticsPanel.cpp
        StatisticsPanel.h
        CommandRegistry.cpp
        CommandRegistry.h
        ConsolePanel.cpp
        ConsolePanel.h
        EditorActions.cpp
        EditorActions.h
        EditorApp.cpp
        EditorApp.h
        EditorCamera.cpp
        EditorCamera.h
        EditorCommands.cpp
        EditorCommands.h
        EditorContext.cpp
        EditorContext.h
        EditorFeedback.h
        EditorIcons.cpp
        EditorIcons.h
        EditorPreferences.cpp
        EditorPreferences.h
        EditorToolbar.cpp
        EditorToolbar.h
        FileAssociation.cpp
        FileAssociation.h
        FileDialogs.cpp
        FileDialogs.h
        GameExport.cpp
        GameExport.h
        NewProject.cpp
        NewProject.h
        Gizmo.cpp
        Gizmo.h
        HierarchyPanel.cpp
        HierarchyPanel.h
        InspectorPanel.cpp
        InspectorPanel.h
        InputCommands.cpp
        InputCommands.h
        PhysicsCommands.cpp
        PhysicsCommands.h
        PlayMode.cpp
        PlayMode.h
        PosedMesh.cpp
        PosedMesh.h
        PropertyEditor.cpp
        PropertyEditor.h
        ReferenceDocs.cpp
        ReferenceDocs.h
        ScriptCommands.cpp
        ScriptCommands.h
        UndoStack.cpp
        UndoStack.h
        ViewportPanel.cpp
        ViewportPanel.h
    DEPENDS
        core
        math
        platform
        assets
        world
        rhi
        renderer
        shaders
        shader_compiler
        ui
        engine)

# Associating project files with the editor writes the registry, and tells the shell.
if(WIN32)
    target_link_libraries(cordite_editor PRIVATE advapi32 shell32)
endif()

# The system's dialogs for files (FileDialogs.cpp). On Windows they are COM objects, which
# nativefiledialog-extended leaves to its user to link.
target_link_libraries(cordite_editor PRIVATE nfd)
if(WIN32)
    target_link_libraries(cordite_editor PRIVATE ole32 uuid)
endif()

# The oldest macOS the programs of the build run on, which the application bundle of an
# exported game says (GameExport.cpp); without it the system reads that from the program.
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    target_compile_definitions(cordite_editor PRIVATE
        CORDITE_MACOS_DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
