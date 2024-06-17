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
target_link_libraries(MeshletViewer 
    tinygltf
    TheForge
)

get_filename_component(SHADER_RESOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/FSL/" ABSOLUTE)
set(DIR_SHADER ${CMAKE_HOME_DIRECTORY}/build/Shaders/)
set(DIR_COMPILEDSHADER ${CMAKE_HOME_DIRECTORY}/build/CompiledShaders/)

add_custom_target(FSL_SHADERS)
IF(CMAKE_SYSTEM_NAME MATCHES "Windows")
    add_custom_command(
        TARGET FSL_SHADERS
        MAIN_DEPENDENCY
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "VULKAN_SDK=$ENV{VULKAN_SDK}" "python3" "${FSL_COMPILER}"
        -l DIRECT3D12 
        -d ${DIR_SHADER}
        --verbose 
        -b  ${DIR_COMPILEDSHADER}
        --incremental 
        --compile ${SHADER_RESOURCE_PATH}/ShaderList.fsl
    )
       
else()
    add_custom_command(
        TARGET FSL_SHADERS
        MAIN_DEPENDENCY
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "VULKAN_SDK=$ENV{VULKAN_SDK}" "python3" "${FSL_COMPILER}"
        -l VULKAN 
        -d ${DIR_SHADER}
        --verbose 
        -b ${DIR_COMPILEDSHADER}
        --incremental 
        --compile ${SHADER_RESOURCE_PATH}/ShaderList.fsl
    )
endif()

add_dependencies(MeshletViewer FSL_SHADERS)
set_output_dir(MeshletViewer "") 


