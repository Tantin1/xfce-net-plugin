cmake_minimum_required(VERSION 3.16)
project(xfce-net-plugin C)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Intl REQUIRED)

pkg_check_modules(GTK3       REQUIRED gtk+-3.0>=3.24)
pkg_check_modules(XFCE4PANEL REQUIRED libxfce4panel-2.0>=4.18)
pkg_check_modules(LIBNOTIFY           libnotify>=0.7)

set(GETTEXT_PACKAGE "xfce-net-plugin")
set(LOCALE_DIR "${CMAKE_INSTALL_PREFIX}/share/locale")

add_definitions(
    -DGETTEXT_PACKAGE="${GETTEXT_PACKAGE}"
    -DLOCALEDIR="${LOCALE_DIR}"
)
if(LIBNOTIFY_FOUND)
    add_definitions(-DHAVE_LIBNOTIFY)
    message(STATUS "libnotify: encontrada, notificaciones activadas")
else()
    message(STATUS "libnotify: NO encontrada, notificaciones desactivadas")
endif()

add_library(xfce-net-plugin MODULE
    plugin.c
    popup.c
    nm-dbus.c
)

target_include_directories(xfce-net-plugin PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${XFCE4PANEL_INCLUDE_DIRS}
    ${LIBNOTIFY_INCLUDE_DIRS}
)

target_compile_options(xfce-net-plugin PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${XFCE4PANEL_CFLAGS_OTHER}
    -Wall -Wextra
)

target_link_libraries(xfce-net-plugin PRIVATE
    ${GTK3_LIBRARIES}
    ${XFCE4PANEL_LIBRARIES}
    ${Intl_LIBRARIES}
    ${LIBNOTIFY_LIBRARIES}
)

if(NOT XFCE_PLUGIN_DIR)
    if(EXISTS "/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins")
        set(XFCE_PLUGIN_DIR "/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins")
    else()
        set(XFCE_PLUGIN_DIR "/usr/lib/xfce4/panel/plugins")
    endif()
endif()
message(STATUS "Plugin install dir: ${XFCE_PLUGIN_DIR}")

install(FILES "${CMAKE_BINARY_DIR}/libxfce-net-plugin.so"
    DESTINATION "${XFCE_PLUGIN_DIR}"
)

# Ícono de candado fallback en hicolor
install(FILES "${CMAKE_SOURCE_DIR}/icons/nm-secure-lock.svg"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps"
)

# Compilar e instalar traducciones
set(LANGS es en de fr pt_BR it nl pl ru zh_CN ja)
foreach(LANG ${LANGS})
    set(PO_FILE  "${CMAKE_SOURCE_DIR}/po/${LANG}.po")
    set(MO_FILE  "${CMAKE_BINARY_DIR}/po/${LANG}.mo")
    add_custom_command(
        OUTPUT  "${MO_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/po"
        COMMAND msgfmt "${PO_FILE}" -o "${MO_FILE}"
        DEPENDS "${PO_FILE}"
    )
    list(APPEND MO_FILES "${MO_FILE}")
    install(FILES "${MO_FILE}"
        DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/${LANG}/LC_MESSAGES"
        RENAME "xfce-net-plugin.mo"
    )
endforeach()
add_custom_target(translations ALL DEPENDS ${MO_FILES})

configure_file(
    "${CMAKE_SOURCE_DIR}/xfce-net-plugin.desktop.in"
    "${CMAKE_BINARY_DIR}/xfce-net-plugin.desktop"
    @ONLY
)

install(FILES "${CMAKE_BINARY_DIR}/xfce-net-plugin.desktop"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/share/xfce4/panel/plugins"
)
