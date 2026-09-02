function(reticulum_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE reticulum::reticulum)
    target_compile_features(${name} PRIVATE c_std_17)
    reticulum_set_project_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

reticulum_add_test(test_buffer test_buffer.c)
reticulum_add_test(test_hal test_hal.c)
reticulum_add_test(test_crypto unit/test_crypto.c)
reticulum_add_test(test_identity unit/test_identity.c)
reticulum_add_test(test_destination unit/test_destination.c)
reticulum_add_test(test_packet unit/test_packet.c)
reticulum_add_test(test_lxmf unit/test_lxmf.c)
reticulum_add_test(test_announce unit/test_announce.c)
reticulum_add_test(test_framing unit/test_framing.c)
reticulum_add_test(test_transport unit/test_transport.c)
reticulum_add_test(test_lxmf_router unit/test_lxmf_router.c)
reticulum_add_test(test_lxmf_router_send unit/test_lxmf_router_send.c)
reticulum_add_test(test_node unit/test_node.c)
reticulum_add_test(test_lxmf_store unit/test_lxmf_store.c)
reticulum_add_test(test_config unit/test_config.c)
reticulum_add_test(test_link unit/test_link.c)
reticulum_add_test(test_request unit/test_request.c)
reticulum_add_test(test_lxmf_delivery unit/test_lxmf_delivery.c)
reticulum_add_test(test_lxmf_peer_store unit/test_lxmf_peer_store.c)
reticulum_add_test(test_ifac unit/test_ifac.c)
reticulum_add_test(test_proof unit/test_proof.c)
reticulum_add_test(test_channel unit/test_channel.c)
reticulum_add_test(test_runtime unit/test_runtime.c)
reticulum_add_test(test_tui_model unit/test_tui_model.c)
target_sources(test_tui_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_model.c)
target_include_directories(test_tui_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_shell unit/test_tui_shell.c)
target_sources(test_tui_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_shell.c)
target_include_directories(test_tui_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_node_registry unit/test_node_registry.c)
reticulum_add_test(test_micron unit/test_micron.c)
reticulum_add_test(test_udp integration/test_udp.c)
reticulum_add_test(test_tcp integration/test_tcp.c)
reticulum_add_test(test_udp_lxmf integration/test_udp_lxmf.c)

if(TARGET rnid)
    add_test(NAME test_rnid
        COMMAND ${CMAKE_COMMAND}
            -DRNID=$<TARGET_FILE:rnid>
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_rnid.cmake)
endif()

if(TARGET nomad-chat)
    add_test(NAME test_nomad_chat
        COMMAND ${CMAKE_COMMAND}
            -DNOMAD_CHAT=$<TARGET_FILE:nomad-chat>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_nomad_chat.cmake)
endif()

if(TARGET nomad-chat)
    add_test(NAME test_tui_cli
        COMMAND ${CMAKE_COMMAND}
            -DNOMAD_CHAT=$<TARGET_FILE:nomad-chat>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_cli.cmake)
endif()
