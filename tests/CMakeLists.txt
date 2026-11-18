# Testes com Qt Test. Cada arquivo tst_*.cpp vira um executável e um teste do CTest.

function(synkro_add_test name)
    qt_add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/support")
    target_link_libraries(${name} PRIVATE Synkro::Core Qt6::Test)
    target_compile_definitions(${name} PRIVATE SYNKRO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    synkro_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    # As DLLs do Qt precisam estar no PATH para o teste rodar fora do Qt Creator.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
