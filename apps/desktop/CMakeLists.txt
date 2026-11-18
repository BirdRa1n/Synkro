qt_add_executable(synkro-desktop
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
)

set_target_properties(synkro-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(synkro-desktop PRIVATE Synkro::Core Qt6::Widgets)
synkro_configure_target(synkro-desktop)

install(TARGETS synkro-desktop RUNTIME DESTINATION bin)
