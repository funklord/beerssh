# =============================================================================
# tests/tests.pro -- host unit tests (project.md sec 12.3)
#
# Built in its own directory from its own .pro, for the reason every Qt
# test project needs one: the test binary needs the app's sources but not
# src/main.cpp, or it would have two main() functions.
#
# No device, no server and no display: these run under the offscreen QPA
# plugin, which is what lets CI check them at all. Run via `make test`
# from the repo root.
# =============================================================================

TEMPLATE = app
TARGET = beerssh_test

QT += widgets network testlib dbus

CONFIG += link_pkgconfig
PKGCONFIG += libsixel libssh libcrypto libsecret-1
CONFIG += c++17 console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../src $$PWD $$PWD/../vterm/include

# Where tools/fetch-fonts.sh puts the curated set (project.md sec 9.2).
# The font tests skip themselves when it is not there, so a checkout that
# has never run the fetch script still has a green test suite.
DEFINES += BSSH_TEST_FONT_DIR=\\\"$$PWD/../fonts\\\"
# forkpty(3) for the local shell transport (sec 2, sec 3); glibc puts
# it in libutil.
LIBS += -lutil

LIBS += $$PWD/../build-vterm/libvterm.a
PRE_TARGETDEPS += $$PWD/../build-vterm/libvterm.a

SOURCES += \
    main.cpp \
    conn_manager_test.cpp \
    emulator_bench.cpp \
    live_ssh_test.cpp \
    emulator_test.cpp \
    graphics_test.cpp \
    platform_test.cpp \
    preset_test.cpp \
    secret_store_test.cpp \
    ui_shots.cpp \
    profile_store_test.cpp \
    session_registry_test.cpp \
    tab_status_test.cpp \
    input_router_test.cpp \
    pty_session_test.cpp \
    layout_test.cpp \
    font_catalog_test.cpp \
    font_picker_test.cpp \
    preset_editor_test.cpp \
    screen_test.cpp \
    ssh_config_test.cpp \
    terminal_view_test.cpp \
    fake_ssh_session.cpp \
    ../src/input/input_router.cpp \
    ../src/ssh/pty_session.cpp \
    ../src/ssh/ssh_session.cpp \
    ../src/ssh/jump_tunnel.cpp \
    ../src/ssh/ssh_session_libssh.cpp \
    ../src/term/emulator_vterm.cpp \
    ../src/term/colour_scheme.cpp \
    ../src/term/font_catalog.cpp \
    ../src/term/graphics.cpp \
    ../src/term/image_store.cpp \
    ../src/term/scrollback.cpp \
    ../src/term/screen.cpp \
    ../src/platform/platform.cpp \
    ../src/platform/secret_store.cpp \
    ../src/platform/secret_store_file.cpp \
    ../src/platform/secret_store_libsecret.cpp \
    ../src/platform/secret_store_select_desktop.cpp \
    ../src/model/app_settings.cpp \
    ../src/model/conn_manager.cpp \
    ../src/model/document_path.cpp \
    ../src/model/preset.cpp \
    ../src/model/ssh_config.cpp \
    ../src/model/profile_store.cpp \
    ../src/model/settings_resolver.cpp \
    ../src/model/session.cpp \
    ../src/model/session_registry.cpp \
    ../src/ui/font_picker.cpp \
    ../src/ui/password_prompt.cpp \
    ../src/ui/preset_editor.cpp \
    ../src/ui/host_editor.cpp \
    ../src/ui/override_badge.cpp \
    ../src/ui/host_list.cpp \
    ../src/ui/layout_manager.cpp \
    ../src/ui/pane_container.cpp \
    ../src/ui/long_press.cpp \
    ../src/ui/wrapped_note.cpp \
    ../src/ui/main_window.cpp \
    ../src/ui/session_dialog.cpp \
    ../src/ui/ssh_config_import.cpp \
    ../src/ui/tab_status.cpp \
    ../src/platform/platform_desktop.cpp \
    ../src/ui/key_row_layout.cpp \
    ../src/ui/app_settings_view.cpp \
    ../src/ui/licences_view.cpp \
    ../src/ui/key_row.cpp \
    ../src/ui/terminal_accessible.cpp \
    ../src/ui/theme.cpp \
    ../src/ui/terminal_view.cpp

HEADERS += \
    fake_ssh_session.h \
    conn_manager_test.h \
    emulator_bench.h \
    live_ssh_test.h \
    emulator_test.h \
    graphics_test.h \
    platform_test.h \
    preset_test.h \
    secret_store_test.h \
    ui_shots.h \
    profile_store_test.h \
    session_registry_test.h \
    tab_status_test.h \
    input_router_test.h \
    pty_session_test.h \
    layout_test.h \
    font_catalog_test.h \
    font_picker_test.h \
    preset_editor_test.h \
    screen_test.h \
    ssh_config_test.h \
    terminal_view_test.h \
    ../src/input/input_router.h \
    ../src/ssh/pty_session.h \
    ../src/ssh/ssh_session.h \
    ../src/ssh/jump_tunnel.h \
    ../src/ssh/ssh_session_libssh.h \
    ../src/term/emulator.h \
    ../src/term/graphics_internal.h \
    ../src/term/image_store.h \
    ../src/term/scrollback.h \
    ../src/term/screen.h \
    ../src/platform/platform.h \
    ../src/platform/secret_store.h \
    ../src/model/profile.h \
    ../src/model/app_settings.h \
    ../src/model/conn_manager.h \
    ../src/model/document_path.h \
    ../src/model/preset.h \
    ../src/model/ssh_config.h \
    ../src/model/profile_store.h \
    ../src/model/settings_resolver.h \
    ../src/model/session.h \
    ../src/model/session_registry.h \
    ../src/ui/font_picker.h \
    ../src/ui/password_prompt.h \
    ../src/ui/preset_editor.h \
    ../src/ui/host_editor.h \
    ../src/ui/override_badge.h \
    ../src/ui/host_list.h \
    ../src/ui/layout_manager.h \
    ../src/ui/pane_container.h \
    ../src/ui/long_press.h \
    ../src/ui/wrapped_note.h \
    ../src/ui/main_window.h \
    ../src/ui/session_dialog.h \
    ../src/ui/ssh_config_import.h \
    ../src/ui/tab_status.h \
    ../src/ui/app_settings_view.h \
    ../src/ui/licences_view.h \
    ../src/ui/key_row.h \
    ../src/ui/terminal_accessible.h \
    ../src/ui/theme.h \
    ../src/ui/terminal_view.h
