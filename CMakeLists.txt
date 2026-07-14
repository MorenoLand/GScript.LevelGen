cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LEVELGEN_VERSION LIMIT_COUNT 1)
string(STRIP "${LEVELGEN_VERSION}" LEVELGEN_VERSION)
project(LevelGen VERSION ${LEVELGEN_VERSION} LANGUAGES CXX RC)

string(REPLACE "." "," LEVELGEN_VERSION_COMMA "${PROJECT_VERSION}")
configure_file(resources/LevelGen.rc.in "${CMAKE_CURRENT_BINARY_DIR}/LevelGen.rc" @ONLY)

add_executable(LevelGen WIN32
    src/LevelGen.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/LevelGen.rc"
)

set_target_properties(LevelGen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
target_compile_features(LevelGen PRIVATE cxx_std_17)
target_compile_definitions(LevelGen PRIVATE LEVELGEN_VERSION="${PROJECT_VERSION}")
target_link_libraries(LevelGen PRIVATE
    user32 gdi32 gdiplus comdlg32 comctl32 shell32 ole32
)

install(TARGETS LevelGen RUNTIME DESTINATION bin)
install(FILES resources/LevelGen-icon.png DESTINATION assets)
install(DIRECTORY docs/ DESTINATION docs)
install(DIRECTORY examples/ DESTINATION examples)
