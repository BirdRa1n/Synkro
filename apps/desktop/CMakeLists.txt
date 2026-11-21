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
target_link_libraries(synkro-desktop PRIVATE Synkro::Core Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::Network)
synkro_configure_target(synkro-desktop)

install(TARGETS synkro-desktop RUNTIME DESTINATION bin)
