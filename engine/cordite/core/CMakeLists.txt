find_package(Threads REQUIRED)

cordite_add_module(core
    SOURCES
        Assert.cpp
        Assert.h
        Base.h
        BinaryStream.cpp
        BinaryStream.h
        Config.cpp
        DirectoryWatcher.cpp
        DirectoryWatcher.h
        Config.h
        Document.cpp
        Document.h
        ExternalAssert.h
        FileSystem.cpp
        FileSystem.h
        Hash.cpp
        Hash.h
        JobSystem.cpp
        JobSystem.h
        Json.cpp
        Json.h
        JsonFwd.h
        Log.cpp
        Log.h
        Platform.cpp
        Platform.h
        Reflection.cpp
        Reflection.h
        Result.cpp
        Result.h
        StringUtils.cpp
        StringUtils.h
        Time.cpp
        Time.h
        Uuid.cpp
        Uuid.h
        memory/LinearAllocator.cpp
        memory/LinearAllocator.h
        memory/Memory.cpp
        memory/Memory.h
        memory/PoolAllocator.cpp
        memory/PoolAllocator.h
    PUBLIC_LIBRARIES
        fmt::fmt
        nlohmann_json::nlohmann_json
        tl::expected
        Threads::Threads
    PRIVATE_LIBRARIES
        spdlog::spdlog
        enkiTS
        cordite::xxhash)

# userConfigurationDirectory() asks the shell for a known folder, which it frees with COM's
# allocator.
if(WIN32)
    target_link_libraries(cordite_core PRIVATE shell32 ole32)
endif()
