cmake_minimum_required(VERSION 3.20)
project(dadlink-client VERSION 2.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# MSVC runtime - use /MD to match vcpkg x64-windows-static-md triplet
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network WebSockets)

# Find vcpkg-managed dependencies
find_package(portaudio CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/config/config.cpp
    src/network/websocket_client.cpp
    src/ui/login_dialog.cpp
    src/ui/register_dialog.cpp
    src/ui/main_window.cpp
    src/ui/channel_tree.cpp
    src/ui/user_list.cpp
    src/ui/activity_log.cpp
    src/crypto/key_exchange.cpp
    src/crypto/srtp_session.cpp
    src/audio/audio_engine.cpp
    src/audio/opus_codec.cpp
    src/voice/voice_packet.cpp
    src/voice/jitter_buffer.cpp
    src/voice/voice_session.cpp
    src/input/ptt_manager.cpp
    src/input/joystick_input.cpp
    src/network/admin_api.cpp
    src/ui/admin/admin_panel.cpp
    src/ui/settings_dialog.cpp
    src/ui/vu_meter.cpp
)

set(HEADERS
    src/config/config.h
    src/network/websocket_client.h
    src/network/admin_api.h
    src/ui/login_dialog.h
    src/ui/register_dialog.h
    src/ui/main_window.h
    src/ui/channel_tree.h
    src/ui/user_list.h
    src/ui/activity_log.h
    src/crypto/key_exchange.h
    src/crypto/srtp_session.h
    src/audio/ring_buffer.h
    src/audio/radio_filter.h
    src/audio/audio_engine.h
    src/audio/opus_codec.h
    src/voice/voice_packet.h
    src/voice/jitter_buffer.h
    src/voice/voice_session.h
    src/input/ptt_manager.h
    src/input/joystick_input.h
    src/ui/admin/admin_panel.h
    src/ui/settings_dialog.h
    src/ui/vu_meter.h
)

# Executable
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Pass version to C++ code
target_compile_definitions(${PROJECT_NAME} PRIVATE
    DADLINK_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    portaudio_static
    Opus::opus
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Windows-specific
if(WIN32)
    # dinput8 + dxguid: joystick/HOTAS PTT (input/joystick_input.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 dinput8 dxguid)
    # Set Windows subsystem (GUI app, no console in release)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE $<CONFIG:Release>
    )
endif()
