cmake_minimum_required(VERSION 3.16)
project(CpuMonitor VERSION 1.4.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# The overlay (see main.cpp, section 8) talks to the desktop through GLib's D-Bus
# functions. Only GLib's headers are needed: the library itself is loaded at
# runtime, so VeeaStats still starts on a system without it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED gio-2.0)

# GLFW is compiled into the program by default, so users don't need it installed.
# GLFW 3.4 loads its X11 and Wayland libraries at runtime, so one binary works
# on both. Pass -DUSE_SYSTEM_GLFW=ON to link the distro's libglfw instead.
option(USE_SYSTEM_GLFW "Link the system GLFW instead of building it" OFF)

if(USE_SYSTEM_GLFW)
    find_package(glfw3 REQUIRED)
else()
    include(FetchContent)
    FetchContent_Declare(glfw
        URL      https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
        URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    )
    set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS       OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL          OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11        ON  CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND    ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

# Collect required Dear ImGui source files
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Executable setup
add_executable(CpuMonitor main.cpp ${IMGUI_SOURCES})

# Include directories for ImGui headers
target_include_directories(CpuMonitor PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GIO_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(CpuMonitor PRIVATE
    OpenGL::GL
    glfw
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Hardening, set here rather than left to each distro's compiler defaults (Ubuntu
# turns most of these on by itself, other distros don't): stack canaries,
# checked string/memory functions, a position-independent program, and a
# read-only symbol table resolved at startup.
include(CheckPIESupported)
check_pie_supported()
set_target_properties(CpuMonitor PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(CpuMonitor PRIVATE -fstack-protector-strong $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2>)
target_link_options(CpuMonitor PRIVATE -Wl,-z,relro -Wl,-z,now)
# Warnings for VeeaStats' own code (not for Dear ImGui's files).
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Bundle libstdc++/libgcc into the binary so it doesn't depend on the build
# machine's (possibly newer) C++ runtime. That matters for the release AppImage,
# which packaging/build-appimage.sh checks; a build for your own PC works either
# way, so it's skipped where the static library isn't installed (for example
# Fedora, unless libstdc++-static is).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
check_cxx_source_compiles("#include <string>
int main() { return static_cast<int>(std::string(\"x\").size()) - 1; }" VEEASTATS_STATIC_LIBSTDCXX)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(VEEASTATS_STATIC_LIBSTDCXX)
    target_link_options(CpuMonitor PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS CpuMonitor RUNTIME DESTINATION bin)
install(FILES packaging/veeastats.desktop DESTINATION share/applications)
install(FILES packaging/veeastats.svg DESTINATION share/icons/hicolor/scalable/apps)
# The skins' embedded fonts are under the SIL Open Font License, which asks
# for the license to travel with the program.
install(FILES fonts/OFL-Orbitron.txt fonts/OFL-Rajdhani.txt fonts/OFL-Michroma.txt fonts/OFL-SairaSemiCondensed.txt
              fonts/OFL-PressStart2P.txt fonts/OFL-Tiny5.txt
              fonts/OFL-HennyPenny.txt fonts/OFL-Fredoka.txt fonts/OFL-Cinzel.txt fonts/OFL-Marcellus.txt
              fonts/OFL-DelaGothicOne.txt fonts/OFL-MPlusRounded1c.txt fonts/OFL-ChakraPetch.txt fonts/OFL-Inter.txt
        DESTINATION share/licenses/veeastats)
