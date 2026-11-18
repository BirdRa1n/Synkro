# libsynkro — motor do Synkro. Depende apenas de Qt Core e de APIs do Windows;
# nunca de Qt Widgets.

add_library(synkro_core STATIC
    catalog/Catalog.h
    catalog/Catalog.cpp
    catalog/Manifest.h
    catalog/Manifest.cpp

    core/Result.h
    core/RunId.h
    core/RunId.cpp
    core/Version.h
    core/Version.cpp

    engine/BackupEngine.h
    engine/BackupEngine.cpp
    engine/Guards.h
    engine/Guards.cpp
    engine/Journal.h
    engine/Journal.cpp
    engine/Recovery.h
    engine/Recovery.cpp
    engine/Restore.h
    engine/Restore.cpp
    engine/Retention.h
    engine/Retention.cpp
    engine/RunSummary.h
    engine/RunSummary.cpp
    engine/VaultOps.h
    engine/VaultOps.cpp
    engine/Verify.h
    engine/Verify.cpp

    hash/Sha256.h
    hash/Sha256.cpp

    platform/windows/AlignedBuffer.h
    platform/windows/Locks.h
    platform/windows/Locks.cpp
    platform/windows/Win32.h
    platform/windows/Win32.cpp
    platform/windows/Volume.h
    platform/windows/Volume.cpp

    scan/ChangeDetector.h
    scan/ChangeDetector.cpp
    scan/FileScanner.h
    scan/FileScanner.cpp
    scan/PathFilter.h
    scan/PathFilter.cpp

    transfer/SafeCopy.h
    transfer/SafeCopy.cpp

    vault/JobLayout.h
    vault/Vault.h
    vault/Vault.cpp
    vault/VaultCandidateCheck.h
    vault/VaultCandidateCheck.cpp
)
add_library(Synkro::Core ALIAS synkro_core)

target_include_directories(synkro_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(synkro_core
    PUBLIC  Qt6::Core
    PRIVATE bcrypt
)
target_compile_definitions(synkro_core PUBLIC
    SYNKRO_VERSION_STRING="${PROJECT_VERSION}"
)
synkro_configure_target(synkro_core)
