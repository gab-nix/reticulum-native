add_library(nomad_qr STATIC tui_qr.c ../third_party/qrcodegen/qrcodegen.c)
target_include_directories(nomad_qr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/qrcodegen)
target_compile_features(nomad_qr PRIVATE c_std_17)
reticulum_set_project_warnings(nomad_qr)
if(RETICULUM_SANITIZERS)
    reticulum_enable_sanitizers(nomad_qr "${RETICULUM_SANITIZERS}")
endif()

add_executable(rnid rnid.c)
target_link_libraries(rnid PRIVATE reticulum::reticulum)
target_compile_features(rnid PRIVATE c_std_17)
reticulum_set_project_warnings(rnid)

install(TARGETS rnid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(nomad-chat
    nomad_chat.c
    tui.c
    tui_editor.c
    tui_interfaces.c
    tui_paths.c
    tui_render.c
    tui_rrc.c
    tui_settings.c
    tui_state.c
    tui_text.c)
target_include_directories(nomad-chat PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(nomad-chat PRIVATE reticulum::reticulum nomad_qr ${CURSES_LIBRARIES})
target_compile_features(nomad-chat PRIVATE c_std_17)
if(UNIX)
    target_compile_definitions(nomad-chat PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(APPLE)
    target_compile_definitions(nomad-chat PRIVATE _DARWIN_C_SOURCE)
endif()
reticulum_set_project_warnings(nomad-chat)

install(TARGETS nomad-chat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ../third_party/qrcodegen/qrcodegen.h
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/reticulum-c/qrcodegen)

add_executable(rnsd rnsd.c)
target_link_libraries(rnsd PRIVATE reticulum::reticulum)
target_compile_features(rnsd PRIVATE c_std_17)
reticulum_set_project_warnings(rnsd)

install(TARGETS rnsd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
