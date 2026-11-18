qt_add_executable(synkro-worker
    main.cpp
)

set_target_properties(synkro-worker PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(synkro-worker PRIVATE Synkro::Core Qt6::Core)
synkro_configure_target(synkro-worker)

install(TARGETS synkro-worker RUNTIME DESTINATION bin)
