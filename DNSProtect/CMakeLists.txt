cmake_minimum_required(VERSION 3.20)
project(dnsprotect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dnsprotect WIN32
    src/app.cpp
    src/config.cpp
    src/dns.cpp
    src/log.cpp
    src/theme.cpp
    src/ui.cpp
    src/common.h
    src/config.h
    src/dns.h
    src/log.h
    src/theme.h
    src/ui.h)

target_compile_definitions(dnsprotect PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(dnsprotect PRIVATE src)

target_link_libraries(dnsprotect PRIVATE
    advapi32 shell32 ole32 uuid iphlpapi ws2_32 comctl32 gdi32 user32 dwmapi)

if (MINGW)
    # MinGW-w64: the toolchain spec auto-links a generic default-manifest.o,
    # so we *replace* it (rather than collide with it) by compiling our full
    # resource script (manifest + version + icon) into a -B search prefix.
    find_program(WINDRES windres REQUIRED)
    set(override_dir ${CMAKE_BINARY_DIR}/mingw-override)
    set(manifest_obj ${override_dir}/default-manifest.o)
    add_custom_command(
        OUTPUT "${manifest_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${override_dir}"
        COMMAND ${WINDRES} -I resources resources/resources.rc -O coff -o "${manifest_obj}"
        DEPENDS resources/resources.rc resources/app.manifest resources/app.ico
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Compiling resources (manifest override)"
        VERBATIM)
    add_custom_target(resources ALL DEPENDS "${manifest_obj}")
    add_dependencies(dnsprotect resources)

    target_compile_options(dnsprotect PRIVATE -municode -Wall)
    target_link_options(dnsprotect PRIVATE -municode -mwindows -static "-B${override_dir}/")
else()
    # MSVC: manifest goes through the linker's manifest tool instead.
    set_source_files_properties(resources/resources.rc
        PROPERTIES COMPILE_FLAGS "/DDZP_NO_MANIFEST")
    target_sources(dnsprotect PRIVATE resources/resources.rc)
    target_compile_options(dnsprotect PRIVATE /W4 /permissive- /EHsc)
    target_link_options(dnsprotect PRIVATE
        /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")
endif()
