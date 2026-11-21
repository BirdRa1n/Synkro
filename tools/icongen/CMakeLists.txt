# Ferramenta de desenvolvimento: regenera assets/icons/synkro.ico e os PNGs.
#   cmake --build --preset mingw-debug --target icongen
#   out\build\mingw-debug\icongen.exe assets\icons\synkro.svg assets\icons\synkro-small.svg assets\icons
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Svg)
qt_add_executable(icongen main.cpp)
set_target_properties(icongen PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(icongen PRIVATE Qt6::Gui Qt6::Svg)
