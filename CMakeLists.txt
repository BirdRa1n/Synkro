cmake_minimum_required(VERSION 3.22)

project(Synkro
    VERSION 0.9.0
    DESCRIPTION "Backup incremental, versionado e redundante para dados críticos"
    LANGUAGES C CXX   # C: SQLite e zstd
)

if(NOT WIN32)
    message(FATAL_ERROR "O Synkro v1 é suportado apenas no Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SYNKRO_BUILD_DESKTOP "Compila o aplicativo desktop (Qt Widgets)" ON)
option(SYNKRO_BUILD_TESTS   "Compila os testes automatizados"           ON)

# Versão completa: 0.9.0 ou, com sufixo de pré-lançamento, 0.9.0-beta.1
# (o workflow de release passa o sufixo a partir da tag).
set(SYNKRO_VERSION_SUFFIX "" CACHE STRING "Sufixo de pré-lançamento da versão (ex.: beta.1)")
if(SYNKRO_VERSION_SUFFIX)
    if(NOT SYNKRO_VERSION_SUFFIX MATCHES "^[0-9A-Za-z.-]+$")
        message(FATAL_ERROR "SYNKRO_VERSION_SUFFIX inválido: ${SYNKRO_VERSION_SUFFIX}")
    endif()
    set(SYNKRO_VERSION_FULL "${PROJECT_VERSION}-${SYNKRO_VERSION_SUFFIX}")
else()
    set(SYNKRO_VERSION_FULL "${PROJECT_VERSION}")
endif()
# Repositório do GitHub consultado pelo atualizador do desktop (vazio = sem atualização automática).
set(SYNKRO_UPDATE_REPO "BirdRa1n/Synkro" CACHE STRING "owner/repo das releases do Synkro")
message(STATUS "Synkro ${SYNKRO_VERSION_FULL}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SynkroCompilerOptions)
include(SynkroDependencies)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)
qt_standard_project_setup()

add_subdirectory(src)
add_subdirectory(apps/worker)

if(SYNKRO_BUILD_DESKTOP)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
    add_subdirectory(apps/desktop)
endif()

if(SYNKRO_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
