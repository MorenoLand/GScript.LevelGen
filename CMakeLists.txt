cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LEVELGEN_VERSION LIMIT_COUNT 1)
string(STRIP "${LEVELGEN_VERSION}" LEVELGEN_VERSION)
project(LevelGen VERSION ${LEVELGEN_VERSION} LANGUAGES CXX)

string(REPLACE "." "," LEVELGEN_VERSION_COMMA "${PROJECT_VERSION}")
if(WIN32)
    enable_language(RC)
    configure_file(resources/LevelGen.rc.in "${CMAKE_CURRENT_BINARY_DIR}/LevelGen.rc" @ONLY)
    add_executable(LevelGen WIN32 src/LevelGen.cpp "${CMAKE_CURRENT_BINARY_DIR}/LevelGen.rc")
else()
    add_executable(LevelGen src/LevelGen.cpp)
endif()

set_target_properties(LevelGen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
target_compile_features(LevelGen PRIVATE cxx_std_17)
target_compile_definitions(LevelGen PRIVATE LEVELGEN_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(LevelGen PRIVATE user32 gdi32 gdiplus comdlg32 comctl32 shell32 ole32)
else()
    find_package(PNG REQUIRED)
    target_link_libraries(LevelGen PRIVATE PNG::PNG)
endif()

install(TARGETS LevelGen RUNTIME DESTINATION .)
install(FILES resources/LevelGen-icon.png DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs)
install(DIRECTORY examples/ DESTINATION examples)
