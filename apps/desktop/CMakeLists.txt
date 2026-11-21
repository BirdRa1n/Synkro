find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent Svg Network)

qt_add_executable(synkro-desktop
    main.cpp
    AppContext.h AppContext.cpp
    Dialogs.h Dialogs.cpp
    Format.h
    Icons.h Icons.cpp
    MainWindow.h MainWindow.cpp
    Pages.h
    DashboardPage.cpp
    HistoryPage.cpp
    JobsPage.cpp
    RestorePage.cpp
    SettingsPage.cpp
    VaultsPage.cpp
    Theme.h Theme.cpp
    Ui.h Ui.cpp
    Updater.h Updater.cpp
)

set_target_properties(synkro-desktop PROPERTIES WIN32_EXECUTABLE TRUE)

# O desktop sempre abre como administrador (UAC). Desligue só para desenvolver
# sem um pedido de UAC a cada execução: -DSYNKRO_DESKTOP_REQUIRE_ADMIN=OFF.
option(SYNKRO_DESKTOP_REQUIRE_ADMIN "Synkro Desktop exige administrador (manifesto)" ON)
if(SYNKRO_DESKTOP_REQUIRE_ADMIN)
    set(SYNKRO_DESKTOP_EXECUTION_LEVEL requireAdministrator)
else()
    set(SYNKRO_DESKTOP_EXECUTION_LEVEL asInvoker)
endif()
set(SYNKRO_DESKTOP_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/synkro-desktop.manifest")
configure_file(synkro-desktop.manifest.in "${SYNKRO_DESKTOP_MANIFEST}" @ONLY)
configure_file(synkro-desktop.rc.in "${CMAKE_CURRENT_BINARY_DIR}/synkro-desktop.rc" @ONLY)
target_sources(synkro-desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/synkro-desktop.rc")
target_compile_definitions(synkro-desktop PRIVATE
    SYNKRO_DESKTOP_REQUIRE_ADMIN=$<BOOL:${SYNKRO_DESKTOP_REQUIRE_ADMIN}>)
target_link_libraries(synkro-desktop PRIVATE Synkro::Core Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::Network)
synkro_configure_target(synkro-desktop)

install(TARGETS synkro-desktop RUNTIME DESTINATION bin)
