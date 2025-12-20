find_package(CLI11 CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)

add_executable(aes67_rx_cli
  src/main.cpp
  src/NmosQueryClient.cpp
  src/RxSession.cpp
  src/PortAudioOutput.cpp
)

target_include_directories(aes67_rx_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(aes67_rx_cli PRIVATE ravennakit)

# vcpkg's portaudio package exports 'portaudio_static' (not 'portaudio').
if (TARGET portaudio_static)
  target_link_libraries(aes67_rx_cli PRIVATE portaudio_static)
elseif (TARGET portaudio)
  target_link_libraries(aes67_rx_cli PRIVATE portaudio)
else ()
  message(FATAL_ERROR "portaudio package found but no known CMake target (expected portaudio_static or portaudio)")
endif ()

# CLI11 target name can vary by packaging.
if (TARGET CLI11::CLI11)
  target_link_libraries(aes67_rx_cli PRIVATE CLI11::CLI11)
elseif (TARGET CLI11)
  target_link_libraries(aes67_rx_cli PRIVATE CLI11)
else ()
  message(FATAL_ERROR "CLI11 package found but no known CMake target (expected CLI11::CLI11 or CLI11)")
endif ()

