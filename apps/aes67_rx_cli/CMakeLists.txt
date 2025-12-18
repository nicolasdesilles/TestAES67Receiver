find_package(CLI11 CONFIG REQUIRED)
find_package(ALSA REQUIRED)

add_executable(aes67_rx_cli
  src/main.cpp
  src/NmosQueryClient.cpp
  src/RxSession.cpp
  src/AlsaOutput.cpp
)

target_include_directories(aes67_rx_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(aes67_rx_cli PRIVATE ravennakit)

# CLI11 target name can vary by packaging.
if (TARGET CLI11::CLI11)
  target_link_libraries(aes67_rx_cli PRIVATE CLI11::CLI11)
elseif (TARGET CLI11)
  target_link_libraries(aes67_rx_cli PRIVATE CLI11)
else ()
  message(FATAL_ERROR "CLI11 package found but no known CMake target (expected CLI11::CLI11 or CLI11)")
endif ()

# ALSA library
if (TARGET ALSA::ALSA)
  target_link_libraries(aes67_rx_cli PRIVATE ALSA::ALSA)
else ()
  # Fallback for older FindALSA behavior
  target_include_directories(aes67_rx_cli PRIVATE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(aes67_rx_cli PRIVATE ${ALSA_LIBRARIES})
endif ()
