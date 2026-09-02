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
reticulum_add_test(test_node unit/test_node.c)
reticulum_add_test(test_lxmf_store unit/test_lxmf_store.c)
reticulum_add_test(test_config unit/test_config.c)
reticulum_add_test(test_udp integration/test_udp.c)
reticulum_add_test(test_tcp integration/test_tcp.c)

if(TARGET rnid)
    add_test(NAME test_rnid
        COMMAND ${CMAKE_COMMAND}
            -DRNID=$<TARGET_FILE:rnid>
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_rnid.cmake)
endif()
