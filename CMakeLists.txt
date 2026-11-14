cmake_minimum_required(VERSION 3.21)
project(CrossTerm VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
file(GLOB VTERM_SOURCES CONFIGURE_DEPENDS vendor/libvterm/src/*.c)
add_library(vterm STATIC ${VTERM_SOURCES})
target_include_directories(vterm PUBLIC vendor/libvterm/include)
set_target_properties(vterm PROPERTIES C_STANDARD 99)
# Monocypher 4.0.3 (BSD-2 or CC0): XChaCha20-Poly1305 and Argon2id for the encrypted sync file (syncvault.cpp).
add_library(monocypher STATIC vendor/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC vendor/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99)

# Terminal-independent logic: profiles, vault, proxy, monitoring, X server, MobaXterm session import.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_library(relaycore STATIC ${SRC}/profiles.cpp ${SRC}/foreign.cpp ${SRC}/credentials.cpp ${SRC}/proxy.cpp
            ${SRC}/monitor.cpp ${SRC}/xserver.cpp ${SRC}/mobaimport.cpp ${SRC}/ftp.cpp ${SRC}/servers.cpp ${SRC}/vnc.cpp ${SRC}/rdp.cpp
            ${SRC}/keepalive.cpp ${SRC}/syncvault.cpp ${SRC}/syncmerge.cpp ${SRC}/syncstorage.cpp)
target_include_directories(relaycore PUBLIC ${SRC})
target_link_libraries(relaycore PUBLIC Qt6::Core Qt6::Widgets Qt6::Network PRIVATE monocypher)
if(WIN32)
    target_link_libraries(relaycore PUBLIC advapi32 bcrypt ole32 oleaut32 uuid ws2_32)
elseif(APPLE)
    # AppKit + objc: askpass helper processes stay out of the Dock (setActivationPolicy, main.cpp).
    # LocalAuthentication: Touch ID before saved passwords are used (touchid.mm).
    enable_language(OBJCXX)
    target_sources(relaycore PRIVATE ${SRC}/touchid.mm)
    set_source_files_properties(${SRC}/touchid.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(relaycore PUBLIC "-framework Security" "-framework CoreFoundation" "-framework AppKit"
                          "-framework LocalAuthentication" objc)
endif()

# Terminal and transports (PTY, telnet, rlogin, serial, mosh).
add_library(termwidget STATIC ${SRC}/terminal.cpp ${SRC}/transports.cpp ${SRC}/mosh.cpp)
target_link_libraries(termwidget PUBLIC relaycore vterm Qt6::Widgets Qt6::Network)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(termwidget PUBLIC util)
elseif(WIN32)
    target_link_libraries(termwidget PUBLIC ws2_32)
endif()

# Application icon: .ico (square tile) in the Windows executable, .icns (rounded macOS tile) in the bundle; PNGs as resources
# for the window icon (crossterm-square.png on Windows, crossterm.png on macOS and Linux).
set(APP_ICON_SOURCES)
if(WIN32)
    list(APPEND APP_ICON_SOURCES assets/crossterm.rc)
elseif(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE crossterm.icns)
    set_source_files_properties(assets/crossterm.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND APP_ICON_SOURCES assets/crossterm.icns)
endif()
add_executable(CrossTerm WIN32 MACOSX_BUNDLE ${SRC}/main.cpp ${APP_ICON_SOURCES})
file(GLOB TRANSLATIONS CONFIGURE_DEPENDS i18n/*.json)
list(FILTER TRANSLATIONS EXCLUDE REGEX "keys\\.json$")
qt_add_resources(CrossTerm "translations" PREFIX "/" BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${TRANSLATIONS})
qt_add_resources(CrossTerm "appicon" PREFIX "/" BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES assets/crossterm.png assets/crossterm-square.png)
target_link_libraries(CrossTerm PRIVATE termwidget)
target_compile_definitions(CrossTerm PRIVATE CROSSTERM_VERSION="${PROJECT_VERSION}")
if(APPLE)
    # Languages listed in Info.plist (CFBundleLocalizations), so macOS shows its own menus and panels in the
    # language the program uses: English plus one entry per i18n/*.json.
    set(CROSSTERM_LOCALIZATIONS "<string>en</string>")
    foreach(translation ${TRANSLATIONS})
        get_filename_component(code "${translation}" NAME_WE)
        string(APPEND CROSSTERM_LOCALIZATIONS "<string>${code}</string>")
    endforeach()
    # Complete Info.plist: bundle identifier (file panels, Keychain), Retina, version, languages.
    set_target_properties(CrossTerm PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.toxiicdev.crossterm"
        MACOSX_BUNDLE_BUNDLE_NAME "CrossTerm"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

enable_testing()
add_executable(profile_tests tests/test.cpp)
target_link_libraries(profile_tests PRIVATE relaycore)
target_compile_definitions(profile_tests PRIVATE CROSSTERM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME profiles COMMAND profile_tests)

# One executable per tests/test_<area>.cpp; windows run offscreen.
file(GLOB AREA_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(source ${AREA_TESTS})
    get_filename_component(name ${source} NAME_WE)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE termwidget)
    target_compile_definitions(${name} PRIVATE CROSSTERM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
