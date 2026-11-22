# libsynkro — motor do Synkro. Depende de Qt Core, Qt Network (só para as
# notificações externas) e de APIs do Windows; nunca de Qt Widgets.

add_library(synkro_core STATIC
    catalog/Catalog.h
    catalog/Catalog.cpp
    catalog/Manifest.h
    catalog/Manifest.cpp

    config/Config.h
    config/ConfigApply.h
    config/ConfigApply.cpp
    config/ConfigLoader.h
    config/ConfigLoader.cpp
    config/ConfigWriter.h
    config/ConfigWriter.cpp
    config/FlowGraph.h
    config/FlowGraph.cpp
    config/VaultAccess.h
    config/VaultAccess.cpp
    config/VaultRemoval.h
    config/VaultRemoval.cpp
    config/ShutdownPolicy.h
    config/ShutdownPolicy.cpp
    config/JobRequest.h
    config/JobRequest.cpp
    config/PlanDescriber.h
    config/PlanDescriber.cpp
    config/Values.h
    config/Values.cpp

    core/Paths.h
    core/Paths.cpp
    core/Result.h
    core/RunId.h
    core/RunId.cpp
    core/SemVer.h
    core/SemVer.cpp
    core/Version.h
    core/Version.cpp

    crypto/Crypto.h
    crypto/Crypto.cpp
    crypto/FileCipher.h
    crypto/FileCipher.cpp
    crypto/VaultKey.h
    crypto/VaultKey.cpp

    database/DatabaseBackup.h
    database/DatabaseBackup.cpp
    database/DatabaseRestore.h
    database/DatabaseRestore.cpp
    database/Drivers.h
    database/Drivers.cpp
    database/DumpStore.h
    database/DumpStore.cpp
    config/ConfigPackage.h
    config/ConfigPackage.cpp
    database/RestoreTest.h
    database/RestoreTest.cpp
    database/Sqlite.h
    database/Sqlite.cpp
    database/SqlClient.h
    database/SqlClient.cpp
    database/Tools.h
    database/Tools.cpp
    database/Zstd.h
    database/Zstd.cpp

    engine/BackupEngine.h
    engine/BackupEngine.cpp
    engine/Guards.h
    engine/Guards.cpp
    engine/Hooks.h
    engine/Hooks.cpp
    engine/Journal.h
    engine/Journal.cpp
    engine/Recovery.h
    engine/Recovery.cpp
    engine/LayoutMigration.h
    engine/LayoutMigration.cpp
    engine/Restore.h
    engine/Restore.cpp
    engine/Retention.h
    engine/Retention.cpp
    engine/RunSummary.h
    engine/RunSummary.cpp
    engine/Scrub.h
    engine/Scrub.cpp
    engine/VaultOps.h
    engine/VaultOps.cpp
    engine/Verify.h
    engine/Verify.cpp

    hash/Sha256.h
    hash/Sha256.cpp

    platform/windows/AlignedBuffer.h
    platform/windows/DiskHealth.h
    platform/windows/DiskHealth.cpp
    platform/windows/NetworkShare.h
    platform/windows/NetworkShare.cpp
    platform/windows/Locks.h
    platform/windows/Locks.cpp
    platform/windows/Shutdown.h
    platform/windows/Shutdown.cpp
    platform/windows/Snapshot.h
    platform/windows/Snapshot.cpp
    platform/windows/TaskScheduler.h
    platform/windows/TaskScheduler.cpp
    platform/windows/Win32.h
    platform/windows/Win32.cpp
    platform/windows/Volume.h
    platform/windows/Volume.cpp

    notify/Notifier.h
    notify/Notifier.cpp
    notify/Transport.cpp

    process/ProcessRunner.h
    process/ProcessRunner.cpp

    secrets/SecretStore.h
    secrets/SecretStore.cpp

    state/Alerts.h
    state/Alerts.cpp
    state/StateStore.h
    state/StateStore.cpp

    support/Doctor.h
    support/Doctor.cpp

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
    vault/VaultLocator.h
    vault/VaultLocator.cpp
)
add_library(Synkro::Core ALIAS synkro_core)

target_include_directories(synkro_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(synkro_core
    PUBLIC  Qt6::Core
    PRIVATE Qt6::Network mpr bcrypt crypt32 advapi32 yaml-cpp::yaml-cpp vssapi ole32 oleaut32 taskschd wbemuuid Synkro::SQLite3 Synkro::Zstd
)
target_compile_definitions(synkro_core PUBLIC
    SYNKRO_VERSION_STRING="${SYNKRO_VERSION_FULL}"
    SYNKRO_UPDATE_REPO="${SYNKRO_UPDATE_REPO}"
)
synkro_configure_target(synkro_core)
