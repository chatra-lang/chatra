add_library(chatra_emb
        EmbeddedLibraries.cpp
        sys.cha
        sysNative.cpp
        format.cha
        formatNative.cpp
        regex.cha
        regexNative_srell.h
        regexNative.cpp
        )

target_link_libraries(chatra_emb chatra_core)
