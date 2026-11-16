# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(SmartRAM LANGUAGES CXX)

add_executable(SmartRAM WIN32
    src/main.cpp
    src/app.cpp
    src/widget.cpp
    src/settings_win.cpp
    src/layerwin.cpp
    src/settings.cpp
    src/metrics.cpp
    src/cleaner.cpp
    src/tray.cpp
    src/gfx.cpp
    res/app.rc
    res/app.manifest)

if (MSVC)
    target_compile_options(SmartRAM PRIVATE
        /W3 /permissive- /Zc:__cplusplus /EHsc /utf-8 /MP)
    target_compile_definitions(SmartRAM PRIVATE UNICODE _UNICODE)
    # statically-linked CRT → no runtime redistribution needed
    set_property(TARGET SmartRAM PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif (MINGW)
    # MinGW-w64 build: no Visual Studio required, and the GCC runtime
    # (libgcc/libstdc++/winpthread) is linked in statically. -municode selects
    # the wWinMain entry point used by main.cpp.
    target_compile_options(SmartRAM PRIVATE -municode)
    target_compile_definitions(SmartRAM PRIVATE UNICODE _UNICODE SMART_RAM_MINGW
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_options(SmartRAM PRIVATE
        -municode -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(SmartRAM PRIVATE
    user32 gdi32 shell32 advapi32 ole32 d2d1 dwrite psapi)
