add_executable(aes67_rx_cli
  src/main.cpp
  src/NmosQueryClient.cpp
  src/RxSession.cpp
  src/PortAudioOutput.cpp
)

target_include_directories(aes67_rx_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(aes67_rx_cli
  PRIVATE
    ravennakit
    CLI11::CLI11
    portaudio_static
)


