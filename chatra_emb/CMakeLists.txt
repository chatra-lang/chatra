add_library(chatra_emb
        EmbeddedLibraries.cpp
        sys.cha
        sysNative.cpp
        )

target_link_libraries(chatra_emb INTERFACE chatra_core)
target_include_directories(chatra_emb PRIVATE ${PROJECT_SOURCE_DIR}/chatra)
