# Dependências de terceiros baixadas no configure (FetchContent), com versão fixa.

include(FetchContent)

# yaml-cpp — leitura da configuração em YAML (docs/configuration.md).
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL       OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

# A 0.8.0 não inclui <cstdint> em emitterutils.cpp, o que quebra no GCC 13+.
if(TARGET yaml-cpp)
    if(MSVC)
        target_compile_options(yaml-cpp PRIVATE /FIcstdint)
    else()
        target_compile_options(yaml-cpp PRIVATE -include cstdint -w)
    endif()
endif()
