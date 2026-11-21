qt_add_executable(synkro-worker
    main.cpp
    Cli.h
    Cli.cpp
    OpsCommands.cpp
)

set_target_properties(synkro-worker PROPERTIES WIN32_EXECUTABLE FALSE)

# Ícone do executável (o mesmo do desktop).
set(SYNKRO_ICON "${PROJECT_SOURCE_DIR}/assets/icons/synkro.ico")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/synkro-worker.rc"
     CONTENT "1 ICON \"${SYNKRO_ICON}\"\n")
target_sources(synkro-worker PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/synkro-worker.rc")
target_link_libraries(synkro-worker PRIVATE Synkro::Core Qt6::Core)
synkro_configure_target(synkro-worker)

install(TARGETS synkro-worker RUNTIME DESTINATION bin)
