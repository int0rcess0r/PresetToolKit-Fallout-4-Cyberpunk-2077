cmake_minimum_required(VERSION 3.16)
project(PresetToolKit VERSION 1.0.0 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

# PresetToolKit - LooksMenu (Fallout 4) preset -> Cyberpunk 2077 player head replacement

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Core library shared by the command-line tool and the GUI
add_library(presettoolkit_core STATIC
  src/Converter.cpp
  src/LooksMenuPreset.cpp
  src/Mapping.cpp
  src/Gltf.cpp
  src/HeadDeformer.cpp
  src/ArchivePackager.cpp
  src/Zip.cpp
  src/ModelExport.cpp
  src/Util.cpp
  src/Fo4Data.cpp
  src/Nif.cpp
  src/Calibrator.cpp
  src/Cr2w.cpp
  src/GameSaves.cpp
  src/PresetFit.cpp
  src/Fo4FaceFit.cpp
  src/Fo4Look.cpp
  src/Tri.cpp
  third_party/miniz/miniz.c
)
target_include_directories(presettoolkit_core PUBLIC src third_party)
# third-party zlib (for Fallout 4 .ba2 archives): build quietly
if(MSVC)
  set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/w")
else()
  set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Command-line tool
add_executable(PresetToolKitCLI src/main.cpp)
target_link_libraries(PresetToolKitCLI PRIVATE presettoolkit_core)

# Windows GUI (file pickers, output folder, log window)
if(WIN32)
  add_executable(PresetToolKit WIN32 src/gui_win32.cpp src/PresetToolKit.rc)
  target_link_libraries(PresetToolKit PRIVATE presettoolkit_core comctl32 comdlg32 ole32 shell32 uuid)
  target_compile_definitions(PresetToolKit PRIVATE UNICODE _UNICODE)
endif()

foreach(t presettoolkit_core PresetToolKitCLI)
  if(MSVC)
    target_compile_options(${t} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/permissive-;/bigobj>")
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${t} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
  endif()
endforeach()
if(WIN32)
  if(MSVC)
    target_compile_options(PresetToolKit PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(PresetToolKit PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    # our .rc already embeds the manifest (visual styles + DPI awareness)
    target_link_options(PresetToolKit PRIVATE /MANIFEST:NO)
  else()
    target_compile_options(PresetToolKit PRIVATE -Wall -Wextra)
    # MinGW: statically link the C++ runtime so the exe runs without extra DLLs
    target_link_options(PresetToolKit PRIVATE -static -municode)
    target_link_options(PresetToolKitCLI PRIVATE -static)
  endif()
endif()

# Older GCC needs stdc++fs for <filesystem>
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(presettoolkit_core PUBLIC stdc++fs)
endif()

# Copy the data folder (mapping, FO4 region files) and the launcher next to the executable.
# A separate always-run target, so edits to data/ are picked up on every build.
add_custom_target(presettoolkit_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:PresetToolKitCLI>/data
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/run_PresetToolKit.bat $<TARGET_FILE_DIR:PresetToolKitCLI>/run_PresetToolKit.bat
  COMMENT "Copying PresetToolKit data files")
add_dependencies(presettoolkit_data PresetToolKitCLI)
if(WIN32)
  add_dependencies(presettoolkit_data PresetToolKit)
endif()

install(TARGETS PresetToolKitCLI RUNTIME DESTINATION bin)
if(WIN32)
  install(TARGETS PresetToolKit RUNTIME DESTINATION bin)
endif()
install(DIRECTORY data/ DESTINATION bin/data)
install(FILES run_PresetToolKit.bat DESTINATION bin)
