add_library(chatra_emb
        EmbeddedLibraries.cpp
        EmbInternal.h
        EmbInternal.cpp
        sys.cha
        sysNative.cpp
        format.cha
        formatNative.cpp
        regex.cha
        regexNative_srell.h
        regexNative.cpp
        containers.cha
        containersNative.h
        containersNative.cpp
        io.cha
        ioNative.cpp
        ioNativeStandardFile.cpp
        )

target_link_libraries(chatra_emb INTERFACE chatra_core)
target_include_directories(chatra_emb PRIVATE ${PROJECT_SOURCE_DIR}/chatra)
