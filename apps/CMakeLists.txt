add_executable(rnid rnid.c)
target_link_libraries(rnid PRIVATE reticulum::reticulum)
target_compile_features(rnid PRIVATE c_std_17)
reticulum_set_project_warnings(rnid)

install(TARGETS rnid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(nomad-chat nomad_chat.c)
target_link_libraries(nomad-chat PRIVATE reticulum::reticulum)
target_compile_features(nomad-chat PRIVATE c_std_17)
reticulum_set_project_warnings(nomad-chat)

install(TARGETS nomad-chat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
