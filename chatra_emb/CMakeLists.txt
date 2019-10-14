add_library(chatra_emb
        EmbeddedLibraries.cpp
        sys.cha
        sysNative.cpp
        )

target_link_libraries(chatra_emb chatra_core)
