# =============================================================================
# test/tests.pro -- host unit tests (project.md sec 12.3)
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

RESOURCES += ../resources.qrc
LIBS += -lz

# The level the app ships at (beerssh.pro): size over speed. The tests ran
# at qmake's -O2 default and so tested code the app never runs -- the
# scrollback rewrap emptied every long line in the -Os app and passed all
# 860 tests at -O2 (2026-10-29, the holder's "almost everything
# disappeared"). Tested as shipped, or not tested.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os
# Strict aliasing off, as in beerssh.pro, which says why.
QMAKE_CXXFLAGS_RELEASE += -fno-strict-aliasing
QMAKE_CFLAGS_RELEASE   -= -O2
QMAKE_CFLAGS_RELEASE   += -Os

# Same define as the app's, and passed in the same way by the top-level
# Makefile. The emulator answers XTVERSION with the program's own name and
# version (sec 7.1), so this file has to carry it or that source will not
# compile here -- which is how an app and its tests stop building from the
# same sources without anybody choosing that.
isEmpty(BSSH_VERSION): BSSH_VERSION = 0.1.0
DEFINES += BSSH_VERSION_STRING=\\\"$$BSSH_VERSION\\\"

CONFIG += link_pkgconfig
PKGCONFIG += libsixel libcrypto libsecret-1 xcb
include($$PWD/../libssh.pri)
CONFIG += c++17 console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../src $$PWD/.. $$PWD $$PWD/../vterm/include

# The on-screen keyboard is softkeys, vendored as a submodule (project.md
# sec 6.2): its .pri adds its sources, its headers for moc, and its
# include/ so "softkeys/keyboard.h" resolves. Not listed here a second
# time, so a file softkeys adds needs no edit in this tree.
include($$PWD/../softkeys/softkeys.pri)

# Where tool/fetch-fonts.sh puts the curated set (project.md sec 9.2).
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
    command_line_test.cpp \
    conn_manager_test.cpp \
    emulator_bench.cpp \
    live_ssh_test.cpp \
    handoff_test.cpp \
    emulator_test.cpp \
    conformance_test.cpp \
    graphics_test.cpp \
    platform_test.cpp \
    preset_test.cpp \
    secret_store_test.cpp \
    ui_shots.cpp \
    profile_store_test.cpp \
    session_registry_test.cpp \
    tab_status_test.cpp \
    host_key_test.cpp \
    input_router_test.cpp \
    pty_session_test.cpp \
    layout_test.cpp \
    font_catalog_test.cpp \
    font_download_test.cpp \
    font_picker_test.cpp \
    preset_editor_test.cpp \
    screen_test.cpp \
    ssh_config_test.cpp \
    sshd_test.cpp \
    sshd_window_test.cpp \
    terminal_view_test.cpp \
    fake_ssh_session.cpp \
    ../src/hang_watchdog.cpp \
    ../src/input/input_router.cpp \
    ../src/ssh/pty_session.cpp \
    ../src/ssh/ssh_session.cpp \
    ../src/ssh/jump_tunnel.cpp \
    ../src/ssh/ssh_session_libssh.cpp \
    ../src/sshd/root_path.cpp \
    ../src/sshd/sftp_handler.cpp \
    ../src/sshd/sshd.cpp \
    ../src/sshd/sshd_platform.cpp \
    ../src/sshd/sshd_platform_desktop.cpp \
    ../src/sshd/sshd_window.cpp \
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
    ../src/model/command_line.cpp \
    ../src/model/document_path.cpp \
    ../src/model/preset.cpp \
    ../src/model/multiplexer.cpp \
    ../src/ui/multiplexer_drawer.cpp \
    ../src/model/ssh_config.cpp \
    ../src/model/profile_store.cpp \
    ../src/model/settings_resolver.cpp \
    ../src/model/window_title.cpp \
    ../src/model/session.cpp \
    ../src/model/handoff_wire.cpp \
    ../src/model/handoff_point.cpp \
    ../src/model/session_registry.cpp \
    ../src/model/kept_scrollback.cpp \
    ../src/model/font_sources.cpp \
    ../src/model/font_download.cpp \
    ../src/term/zip_index.cpp \
    ../src/term/picture_spill.cpp \
    ../src/model/custom_font.cpp \
    ../src/ui/font_picker.cpp \
    ../src/ui/form_wrap.cpp \
    ../src/ui/password_prompt.cpp \
    ../src/ui/preset_editor.cpp \
    ../src/ui/host_editor.cpp \
    ../src/ui/override_badge.cpp \
    ../src/ui/host_list.cpp \
    ../src/ui/layout_manager.cpp \
    ../src/ui/pane_bar.cpp \
    ../src/ui/pane_container.cpp \
    ../src/ui/find_bar.cpp \
    ../src/ui/input_selector.cpp \
    ../src/ui/interface_scale.cpp \
    ../src/ui/long_press.cpp \
    ../src/term/term_features.cpp \
    ../src/ui/scroll_guard.cpp \
    ../src/ui/wrapped_note.cpp \
    ../src/ui/main_window.cpp \
    ../src/ui/session_dialog.cpp \
    ../src/ui/ssh_config_import.cpp \
    ../src/ui/tab_status.cpp \
    ../src/ui/touch_metrics.cpp \
    ../src/platform/platform_desktop.cpp \
    ../src/ui/app_settings_view.cpp \
    ../src/ui/store_view.cpp \
    ../src/ui/colour_editor.cpp \
    ../src/ui/term_features_editor.cpp \
    ../src/ui/key_row_editor.cpp \
    ../src/ui/licences_view.cpp \
    ../src/ui/terminal_accessible.cpp \
    ../src/ui/theme.cpp \
    ../src/ui/terminal_view.cpp

HEADERS += \
    fake_ssh_session.h \
    phone_metrics.h \
    command_line_test.h \
    conn_manager_test.h \
    emulator_bench.h \
    live_ssh_test.h \
    handoff_test.h \
    emulator_test.h \
    conformance_test.h \
    graphics_test.h \
    platform_test.h \
    preset_test.h \
    secret_store_test.h \
    ui_shots.h \
    profile_store_test.h \
    session_registry_test.h \
    tab_status_test.h \
    host_key_test.h \
    input_router_test.h \
    pty_session_test.h \
    layout_test.h \
    font_catalog_test.h \
    font_download_test.h \
    font_picker_test.h \
    preset_editor_test.h \
    screen_test.h \
    ssh_config_test.h \
    sshd_test.h \
    sshd_window_test.h \
    terminal_view_test.h \
    ../src/hang_watchdog.h \
    ../src/input/input_router.h \
    ../src/ssh/pty_session.h \
    ../src/ssh/ssh_session.h \
    ../src/ssh/jump_tunnel.h \
    ../src/ssh/ssh_session_libssh.h \
    ../src/sshd/root_path.h \
    ../src/sshd/sftp_handler.h \
    ../src/sshd/sshd.h \
    ../src/sshd/sshd_platform.h \
    ../src/sshd/sshd_window.h \
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
    ../src/model/command_line.h \
    ../src/model/document_path.h \
    ../src/model/preset.h \
    ../src/model/multiplexer.h \
    ../src/ui/multiplexer_drawer.h \
    ../src/model/ssh_config.h \
    ../src/model/profile_store.h \
    ../src/model/settings_resolver.h \
    ../src/model/window_title.h \
    ../src/model/session.h \
    ../src/model/handoff_wire.h \
    ../src/model/handoff_point.h \
    ../src/model/session_registry.h \
    ../src/model/kept_scrollback.h \
    ../src/model/font_sources.h \
    ../src/model/font_download.h \
    ../src/term/zip_index.h \
    ../src/term/picture_spill.h \
    ../src/model/custom_font.h \
    ../src/ui/font_picker.h \
    ../src/ui/form_wrap.h \
    ../src/ui/password_prompt.h \
    ../src/ui/preset_editor.h \
    ../src/ui/host_editor.h \
    ../src/ui/override_badge.h \
    ../src/ui/host_list.h \
    ../src/ui/layout_manager.h \
    ../src/ui/pane_bar.h \
    ../src/ui/pane_container.h \
    ../src/ui/pane_snap.h \
    ../src/ui/find_bar.h \
    ../src/ui/input_selector.h \
    ../src/ui/input_mode.h \
    ../src/ui/interface_scale.h \
    ../src/platform/headless.h \
    ../src/ui/long_press.h \
    ../src/term/term_features.h \
    ../src/ui/scroll_guard.h \
    ../src/ui/wrapped_note.h \
    ../src/ui/main_window.h \
    ../src/ui/session_dialog.h \
    ../src/ui/ssh_config_import.h \
    ../src/ui/tab_status.h \
    ../src/ui/touch_metrics.h \
    ../src/ui/app_settings_view.h \
    ../src/ui/store_view.h \
    ../src/ui/colour_editor.h \
    ../src/ui/term_features_editor.h \
    ../src/ui/key_row_editor.h \
    ../src/ui/licences_view.h \
    ../src/ui/terminal_accessible.h \
    ../src/ui/theme.h \
    ../src/ui/terminal_view.h
