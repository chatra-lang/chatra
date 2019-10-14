add_library(chatra_core
        Internal.h
        StringTable.h
        StringTable.cpp
        LexicalAnalyzer.h
        LexicalAnalyzer.cpp
        Parser.h
        Parser.cpp
        MemoryManagement.h
        MemoryManagement.cpp
        Classes.h
        Classes.cpp
        Runtime.h
        Runtime.cpp
        TemporaryObject.cpp
        Miscellaneous.cpp
        Thread.cpp
        EmbeddedFunctions.cpp
        EmbeddedClasses.cpp
        NativeCall.cpp
        Timer.h
        Timer.cpp
        Serialize.h
        Serialize.cpp
        )

target_include_directories(chatra_core PUBLIC ${PROJECT_SOURCE_DIR}/chatra)
