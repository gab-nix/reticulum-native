function(reticulum_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE reticulum::reticulum)
    target_compile_features(${name} PRIVATE c_std_17)
    reticulum_set_project_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Explicit opt-in Python interoperability harness; not part of offline CTest.
add_executable(lxmf_direct_live_driver interop/lxmf_direct_live_driver.c)
target_link_libraries(lxmf_direct_live_driver PRIVATE reticulum::reticulum)
target_compile_features(lxmf_direct_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(lxmf_direct_live_driver)

# Explicit opt-in stock-Python propagation acceptance driver.
add_executable(lxmf_propagation_live_driver interop/lxmf_propagation_live_driver.c)
target_link_libraries(lxmf_propagation_live_driver PRIVATE reticulum::reticulum)
target_compile_features(lxmf_propagation_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(lxmf_propagation_live_driver)

reticulum_add_test(test_buffer test_buffer.c)
reticulum_add_test(test_hal test_hal.c)
reticulum_add_test(test_crypto unit/test_crypto.c)
reticulum_add_test(test_identity unit/test_identity.c)
reticulum_add_test(test_ratchet_store unit/test_ratchet_store.c)
reticulum_add_test(test_destination unit/test_destination.c)
reticulum_add_test(test_packet unit/test_packet.c)
reticulum_add_test(test_lxmf unit/test_lxmf.c)
reticulum_add_test(test_lxmf_stamp_job unit/test_lxmf_stamp_job.c)
reticulum_add_test(test_lxmf_paper unit/test_lxmf_paper.c)
reticulum_add_test(test_lxmf_uri_fuzz unit/test_lxmf_uri_fuzz.c)
target_sources(test_lxmf_uri_fuzz PRIVATE fuzz/fuzz_lxmf_uri.c)
reticulum_add_test(test_announce unit/test_announce.c)
reticulum_add_test(test_framing unit/test_framing.c)
reticulum_add_test(test_transport unit/test_transport.c)
reticulum_add_test(test_lxmf_router unit/test_lxmf_router.c)
reticulum_add_test(test_lxmf_python_announce interop/test_lxmf_python_announce.c)
reticulum_add_test(test_lxmf_python_propagation interop/test_lxmf_python_propagation.c)
reticulum_add_test(test_lxmf_tickets unit/test_lxmf_tickets.c)
reticulum_add_test(test_lxmf_python_message interop/test_lxmf_python_message.c)
reticulum_add_test(test_lxmf_router_send unit/test_lxmf_router_send.c)
reticulum_add_test(test_lxmf_router_signature unit/test_lxmf_router_signature.c)
reticulum_add_test(test_lxmf_router_stamps unit/test_lxmf_router_stamps.c)
reticulum_add_test(test_lxmf_router_stamp_queue unit/test_lxmf_router_stamp_queue.c)
reticulum_add_test(test_lxmf_router_policy unit/test_lxmf_router_policy.c)
reticulum_add_test(test_lxmf_router_paper unit/test_lxmf_router_paper.c)
reticulum_add_test(test_node unit/test_node.c)
reticulum_add_test(test_lxmf_store unit/test_lxmf_store.c)
reticulum_add_test(test_lxmf_store_delivery unit/test_lxmf_store_delivery.c)
reticulum_add_test(test_lxmf_store_signature unit/test_lxmf_store_signature.c)
reticulum_add_test(test_lxmf_store_large unit/test_lxmf_store_large.c)
reticulum_add_test(test_config unit/test_config.c)
reticulum_add_test(test_link unit/test_link.c)
reticulum_add_test(test_rns_python_link interop/test_rns_python_link.c)
reticulum_add_test(test_request unit/test_request.c)
reticulum_add_test(test_resource unit/test_resource.c)
reticulum_add_test(test_browser unit/test_browser.c)
reticulum_add_test(test_lxmf_delivery unit/test_lxmf_delivery.c)
reticulum_add_test(test_lxmf_peer_store unit/test_lxmf_peer_store.c)
reticulum_add_test(test_ifac unit/test_ifac.c)
reticulum_add_test(test_proof unit/test_proof.c)
reticulum_add_test(test_channel unit/test_channel.c)
reticulum_add_test(test_runtime unit/test_runtime.c)
reticulum_add_test(test_tui_text unit/test_tui_text.c)
target_sources(test_tui_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
target_include_directories(test_tui_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_editor unit/test_tui_editor.c)
target_sources(test_tui_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
target_include_directories(test_tui_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_state unit/test_tui_state.c)
target_sources(test_tui_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
target_include_directories(test_tui_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_settings unit/test_tui_settings.c)
target_sources(test_tui_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
target_include_directories(test_tui_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_rrc unit/test_tui_rrc.c)
target_sources(test_tui_rrc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
target_include_directories(test_tui_rrc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_node_registry unit/test_node_registry.c)
reticulum_add_test(test_micron unit/test_micron.c)
reticulum_add_test(test_rrc unit/test_rrc.c)
reticulum_add_test(test_rrc_session unit/test_rrc_session.c)
reticulum_add_test(test_nomadnet_python_rrc interop/test_nomadnet_python_rrc.c)
reticulum_add_test(test_udp integration/test_udp.c)
reticulum_add_test(test_tcp integration/test_tcp.c)
reticulum_add_test(test_runtime_tcp_reconnect integration/test_runtime_tcp_reconnect.c)
reticulum_add_test(test_udp_lxmf integration/test_udp_lxmf.c)
reticulum_add_test(test_runtime_receipt integration/test_runtime_receipt.c)
reticulum_add_test(test_lxmf_router_receipt integration/test_lxmf_router_receipt.c)
reticulum_add_test(test_lxmf_router_recovery unit/test_lxmf_router_recovery.c)
reticulum_add_test(test_lxmf_router_direct integration/test_lxmf_router_direct.c)
reticulum_add_test(test_runtime_inbound_link integration/test_runtime_inbound_link.c)
reticulum_add_test(test_resource_sender integration/test_resource_sender.c)
reticulum_add_test(test_runtime_resource integration/test_runtime_resource.c)
reticulum_add_test(test_rrc_session_runtime integration/test_rrc_session_runtime.c)
reticulum_add_test(test_lxmf_propagation_session integration/test_lxmf_propagation_session.c)
reticulum_add_test(test_lxmf_router_propagation integration/test_lxmf_router_propagation.c)
reticulum_add_test(test_lxmf_router_propagation_sync integration/test_lxmf_router_propagation_sync.c)
reticulum_add_test(test_hosted_node unit/test_hosted_node.c)
reticulum_add_test(test_hosted_form unit/test_hosted_form.c)
reticulum_add_test(test_hosted_node_link integration/test_hosted_node_link.c)

if(TARGET rnid)
    add_test(NAME test_rnid
        COMMAND ${CMAKE_COMMAND}
            -DRNID=$<TARGET_FILE:rnid>
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_rnid.cmake)
endif()

if(TARGET nomad-chat)
    add_executable(test_tui_settings_ui
        unit/test_tui_settings_ui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
    target_include_directories(test_tui_settings_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps ${CURSES_INCLUDE_DIRS})
    target_link_libraries(test_tui_settings_ui PRIVATE
        reticulum::reticulum ${CURSES_LIBRARIES})
    target_compile_features(test_tui_settings_ui PRIVATE c_std_17)
    reticulum_set_project_warnings(test_tui_settings_ui)
    add_test(NAME test_tui_settings_ui COMMAND test_tui_settings_ui)

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

if(TARGET nomad-chat)
    add_executable(test_tui_dispatch
        unit/test_tui_dispatch.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
    target_include_directories(test_tui_dispatch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps ${CURSES_INCLUDE_DIRS})
    target_link_libraries(test_tui_dispatch PRIVATE
        reticulum::reticulum ${CURSES_LIBRARIES})
    target_compile_features(test_tui_dispatch PRIVATE c_std_17)
    reticulum_set_project_warnings(test_tui_dispatch)
    add_test(NAME test_tui_dispatch COMMAND test_tui_dispatch)
endif()
