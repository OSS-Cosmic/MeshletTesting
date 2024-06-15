cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(INSTALL_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(CMAKE_INSTALL_PREFIX ${INSTALL_OUTPUT_PATH} CACHE PATH "Install path prefix, prepended onto install directories." FORCE)
set(CMAKE_MODULE_PATH ${CMAKE_HOME_DIRECTORY}/cmake)

enable_language(ASM)

if(MSVC)
    add_definitions(/MP /std:c++20)
endif()

include(utils)
add_subdirectory(external)

file(GLOB MESHLET_VIEWER_SRC 
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(MeshletViewer ${MESHLET_VIEWER_SRC})
target_link_libraries(MeshletViewer TheForge)
set_output_dir(MeshletViewer "") 


