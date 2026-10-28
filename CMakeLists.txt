cmake_minimum_required(VERSION 3.21)
project(PulsifyWeb VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# The server only needs QtCore + QtNetwork (no Widgets, no Multimedia, no Gui).
find_package(Qt6 REQUIRED COMPONENTS Core Network)

qt_add_executable(pulsify-server
    server/main.cpp
    server/httpserver.h
    server/online.h
    server/relay.h
    server/playlists.h
    server/google.h
    server/discord.h
    server/lyrics.h
    server/library.h
    web.qrc
)

target_link_libraries(pulsify-server PRIVATE Qt6::Core Qt6::Network)

# Optional: bake your Discord Application ID in:  cmake -DPULSE_DISCORD_CLIENT_ID=123...
if(PULSE_DISCORD_CLIENT_ID)
    target_compile_definitions(pulsify-server PRIVATE PULSE_DISCORD_CLIENT_ID="${PULSE_DISCORD_CLIENT_ID}")
endif()

if(WIN32)
    # console subsystem on purpose: closing the window stops the server
    target_link_libraries(pulsify-server PRIVATE ws2_32)
endif()
