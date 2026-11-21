# Dependências de terceiros baixadas no configure (FetchContent), com versão fixa.

include(FetchContent)

# O CMake 4 recusa projetos com cmake_minimum_required abaixo de 3.5, como o
# yaml-cpp 0.8.0 (3.4). Vale só para as dependências; versões anteriores do
# CMake ignoram esta variável.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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

# SQLite — banco local de estado (synkro.db) e Online Backup API para
# tarefas de banco SQLite. Amalgamation com hash fixo.
FetchContent_Declare(sqlite3
    URL      https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823CB110929C2BCB0F5D48E4833B5C59A8A6E40CDEA3936B99E199DBBE5784
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)
add_library(synkro_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
add_library(Synkro::SQLite3 ALIAS synkro_sqlite3)
target_include_directories(synkro_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(synkro_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_FOREIGN_KEYS=1
)
if(NOT MSVC)
    target_compile_options(synkro_sqlite3 PRIVATE -w)
endif()

# zstd — compressão em fluxo dos dumps de banco.
set(ZSTD_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED         OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC         ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT       OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  build/cmake
)
FetchContent_MakeAvailable(zstd)
add_library(synkro_zstd INTERFACE)
add_library(Synkro::Zstd ALIAS synkro_zstd)
target_link_libraries(synkro_zstd INTERFACE libzstd_static)
target_include_directories(synkro_zstd SYSTEM INTERFACE "${zstd_SOURCE_DIR}/lib")

# A 0.8.0 não inclui <cstdint> em emitterutils.cpp, o que quebra no GCC 13+.
if(TARGET yaml-cpp)
    if(MSVC)
        target_compile_options(yaml-cpp PRIVATE /FIcstdint)
    else()
        target_compile_options(yaml-cpp PRIVATE -include cstdint -w)
    endif()
endif()
