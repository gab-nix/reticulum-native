if(UNIX)
    # Strict C17 disables POSIX declarations unless a feature-test macro is
    # requested before system headers. The test suite uses mkstemp(),
    # mkdtemp(), nanosleep(), and related POSIX.1-2008 helpers.
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

function(reticulum_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE reticulum::reticulum)
    target_compile_features(${name} PRIVATE c_std_17)
    reticulum_set_project_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
reticulum_add_test(test_runtime_provider unit/test_runtime_provider.c)
reticulum_add_test(test_button_menu unit/test_button_menu.c)
reticulum_add_test(test_chat_journal unit/test_chat_journal.c)
target_sources(test_chat_journal PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/chat_journal.c)
target_include_directories(test_chat_journal PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_chat_store unit/test_chat_store.c)
target_sources(test_chat_store PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/chat_store.c)
target_include_directories(test_chat_store PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_channel_view unit/test_channel_view.c)
target_sources(test_channel_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/channel_view.c)
target_include_directories(test_channel_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_cpu_usage unit/test_cpu_usage.c)
target_sources(test_cpu_usage PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/cpu_usage.c)
target_include_directories(test_cpu_usage PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_home_view unit/test_home_view.c)
target_sources(test_home_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/home_view.c)
target_include_directories(test_home_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_live_view unit/test_live_view.c)
target_sources(test_live_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/live_view.c)
target_include_directories(test_live_view PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
target_sources(test_button_menu PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/button_menu.c)
target_include_directories(test_button_menu PRIVATE ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_lxmf_packet_node unit/test_lxmf_packet_node.c)
reticulum_add_test(test_announce_button unit/test_announce_button.c)
target_sources(test_announce_button PRIVATE
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/announce_button.c)
target_include_directories(test_announce_button PRIVATE
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
reticulum_add_test(test_heltec_discovery unit/test_heltec_discovery.c)
target_sources(test_heltec_discovery PRIVATE
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/radio_discovery.c)
target_include_directories(test_heltec_discovery PRIVATE
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main)
add_executable(test_heltec_bringup unit/test_heltec_bringup.c
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/bringup.c
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main/radio_discovery.c)
target_link_libraries(test_heltec_bringup PRIVATE reticulum::reticulum)
target_include_directories(test_heltec_bringup PRIVATE
    heltec_bringup_fakes ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/main
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/components/heltec_status_ui/include
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/include
    ${PROJECT_SOURCE_DIR}/firmware/heltec_wifi_lora_32_v3_1/components/heltec_v3_1_bsp/include)
target_compile_definitions(test_heltec_bringup PRIVATE ESP_PLATFORM)
target_compile_features(test_heltec_bringup PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_bringup)
add_test(NAME test_heltec_bringup COMMAND test_heltec_bringup)
add_test(NAME test_heltec_boot_config COMMAND ${CMAKE_COMMAND}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/heltec_boot_config.cmake)

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

# Explicit opt-in pinned-Python RRC protocol fixture hub acceptance driver.
add_executable(rrc_live_driver interop/rrc_live_driver.c)
target_link_libraries(rrc_live_driver PRIVATE reticulum::reticulum)
target_compile_features(rrc_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(rrc_live_driver)
add_executable(browser_live_driver interop/browser_live_driver.c)
target_link_libraries(browser_live_driver PRIVATE reticulum::reticulum)
target_compile_features(browser_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(browser_live_driver)
add_executable(hosted_live_driver interop/hosted_live_driver.c)
target_link_libraries(hosted_live_driver PRIVATE reticulum::reticulum)
target_compile_features(hosted_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(hosted_live_driver)
add_executable(discovery_live_driver interop/discovery_live_driver.c)
target_link_libraries(discovery_live_driver PRIVATE reticulum::reticulum)
target_compile_features(discovery_live_driver PRIVATE c_std_17)
reticulum_set_project_warnings(discovery_live_driver)

option(RETICULUM_PYTHON_INTEROP "Run opt-in pinned Python interoperability tests" OFF)
if(RETICULUM_PYTHON_INTEROP)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    set(RETICULUM_PYTHON_RNS "" CACHE PATH "Pinned Reticulum source checkout")
    set(RETICULUM_PYTHON_LXMF "" CACHE PATH "Pinned LXMF source checkout")
    set(RETICULUM_PYTHON_NOMADNET "" CACHE PATH "Pinned NomadNet source checkout")
    foreach(checkout RNS LXMF NOMADNET)
        if(NOT IS_DIRECTORY "${RETICULUM_PYTHON_${checkout}}")
            message(FATAL_ERROR "Set RETICULUM_PYTHON_${checkout} to its pinned source checkout")
        endif()
    endforeach()
    foreach(transport udp tcp)
        add_test(NAME interop_discovery_${transport}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_discovery_live.py
                --reticulum ${RETICULUM_PYTHON_RNS}
                --driver $<TARGET_FILE:discovery_live_driver> --transport ${transport})
        set_tests_properties(interop_discovery_${transport} PROPERTIES
            LABELS "interop;python;discovery;${transport}" TIMEOUT 90 RUN_SERIAL TRUE)
        add_test(NAME interop_hosted_${transport}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_hosted_live.py
                --reticulum ${RETICULUM_PYTHON_RNS}
                --driver $<TARGET_FILE:hosted_live_driver> --transport ${transport})
        set_tests_properties(interop_hosted_${transport} PROPERTIES
            LABELS "interop;python;hosting;${transport}" TIMEOUT 150 RUN_SERIAL TRUE)
        add_test(NAME interop_browser_${transport}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_browser_live.py
                --reticulum ${RETICULUM_PYTHON_RNS}
                --nomadnet ${RETICULUM_PYTHON_NOMADNET}
                --driver $<TARGET_FILE:browser_live_driver> --transport ${transport})
        set_tests_properties(interop_browser_${transport} PROPERTIES
            LABELS "interop;python;browser;${transport}" TIMEOUT 120 RUN_SERIAL TRUE)
        foreach(mode direct propagation)
            set(test_name interop_lxmf_${mode}_${transport})
            add_test(NAME ${test_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tools/test_lxmf_${mode}_live.py
                    --reticulum ${RETICULUM_PYTHON_RNS}
                    --lxmf ${RETICULUM_PYTHON_LXMF}
                    --driver $<TARGET_FILE:lxmf_${mode}_live_driver>
                    --transport ${transport})
            set_tests_properties(${test_name} PROPERTIES
                LABELS "interop;python;${transport}" TIMEOUT 600 RUN_SERIAL TRUE)
        endforeach()
    endforeach()
    add_test(NAME interop_lxmf_ticket_worker_race_udp
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_lxmf_direct_live.py
            --reticulum ${RETICULUM_PYTHON_RNS}
            --lxmf ${RETICULUM_PYTHON_LXMF}
            --driver $<TARGET_FILE:lxmf_direct_live_driver>
            --transport udp --delay-inbound-ms 500)
    set_tests_properties(interop_lxmf_ticket_worker_race_udp PROPERTIES
        LABELS "interop;python;udp;ticket;adversarial" TIMEOUT 600 RUN_SERIAL TRUE)
    add_test(NAME interop_rrc_python
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_rrc_live.py
            --reticulum ${RETICULUM_PYTHON_RNS}
            --nomadnet ${RETICULUM_PYTHON_NOMADNET}
            --driver $<TARGET_FILE:rrc_live_driver>)
    set_tests_properties(interop_rrc_python PROPERTIES
        LABELS "interop;python;rrc" TIMEOUT 600 RUN_SERIAL TRUE)
endif()

reticulum_add_test(test_buffer test_buffer.c)
reticulum_add_test(test_hal test_hal.c)
reticulum_add_test(test_platform_api unit/test_platform_api.c)
reticulum_add_test(test_storage_record unit/test_storage_record.c)

add_executable(test_esp_delay
    unit/test_esp_delay.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf/esp_delay.c)
target_include_directories(test_esp_delay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf)
target_link_libraries(test_esp_delay PRIVATE reticulum::reticulum)
target_compile_features(test_esp_delay PRIVATE c_std_17)
reticulum_set_project_warnings(test_esp_delay)
add_test(NAME test_esp_delay COMMAND test_esp_delay)

add_executable(test_esp_nvs_storage
    unit/test_esp_nvs_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf/nvs_storage.c)
target_include_directories(test_esp_nvs_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf/include)
target_link_libraries(test_esp_nvs_storage PRIVATE reticulum::reticulum Threads::Threads)
target_compile_features(test_esp_nvs_storage PRIVATE c_std_17)
reticulum_set_project_warnings(test_esp_nvs_storage)
add_test(NAME test_esp_nvs_storage COMMAND test_esp_nvs_storage)

add_executable(test_platform_no_default
    unit/test_platform_no_default.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/interface.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/storage.c)
target_include_directories(test_platform_no_default PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(test_platform_no_default PRIVATE c_std_17)
reticulum_set_project_warnings(test_platform_no_default)
add_test(NAME test_platform_no_default COMMAND test_platform_no_default)

# The descriptor deliberately has no ESP-IDF dependency, allowing the desktop
# build to catch pin and display power-sequencing regressions.
add_executable(test_heltec_v3_1_bsp
    unit/test_heltec_v3_1_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_v3_1_bsp/heltec_wifi_lora_32_v3_1.c)
target_include_directories(test_heltec_v3_1_bsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_v3_1_bsp/include)
target_compile_features(test_heltec_v3_1_bsp PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_v3_1_bsp)
add_test(NAME test_heltec_v3_1_bsp COMMAND test_heltec_v3_1_bsp)

add_executable(test_heltec_status_ui
    unit/test_heltec_status_ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_status_ui/heltec_status_ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_v3_1_bsp/heltec_wifi_lora_32_v3_1.c)
target_include_directories(test_heltec_status_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_status_ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_v3_1_bsp/include)
target_compile_features(test_heltec_status_ui PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_status_ui)
add_test(NAME test_heltec_status_ui COMMAND test_heltec_status_ui)

add_executable(test_heltec_shell_core
    unit/test_heltec_shell_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_shell_core/heltec_shell_core.c)
target_include_directories(test_heltec_shell_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_shell_core/include)
target_link_libraries(test_heltec_shell_core PRIVATE reticulum::reticulum)
target_compile_features(test_heltec_shell_core PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_shell_core)
add_test(NAME test_heltec_shell_core COMMAND test_heltec_shell_core)

# Compile the exact vendored Semtech sources on the host and exercise their
# public command/HAL boundary. This is not a substitute for an ESP-IDF build or
# physical SX1262 validation.
add_library(semtech_sx126x_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/semtech/sx126x_driver/src/sx126x.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/semtech/sx126x_driver/src/sx126x_driver_version.c)
target_include_directories(semtech_sx126x_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/semtech/sx126x_driver/src)
target_compile_features(semtech_sx126x_host PUBLIC c_std_17)
# Upstream is compiled unchanged. Keep standard warning groups fatal while not
# imposing this project's additional conversion policy on third-party code.
target_compile_options(semtech_sx126x_host PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_executable(test_semtech_sx126x unit/test_semtech_sx126x.c)
target_link_libraries(test_semtech_sx126x PRIVATE semtech_sx126x_host)
target_compile_features(test_semtech_sx126x PRIVATE c_std_17)
reticulum_set_project_warnings(test_semtech_sx126x)
add_test(NAME test_semtech_sx126x COMMAND test_semtech_sx126x)

add_executable(test_heltec_sx1262_radio unit/test_heltec_sx1262_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/sx1262_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/sx1262_radio.c)
target_include_directories(test_heltec_sx1262_radio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262)
target_link_libraries(test_heltec_sx1262_radio PRIVATE reticulum::reticulum)
target_compile_features(test_heltec_sx1262_radio PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_sx1262_radio)
add_test(NAME test_heltec_sx1262_radio COMMAND test_heltec_sx1262_radio)

add_executable(test_heltec_sx1262_semtech unit/test_heltec_sx1262_semtech.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/sx1262_semtech.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/sx1262_radio.c)
target_include_directories(test_heltec_sx1262_semtech PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/include)
target_link_libraries(test_heltec_sx1262_semtech PRIVATE reticulum::reticulum semtech_sx126x_host)
target_compile_features(test_heltec_sx1262_semtech PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_sx1262_semtech)
add_test(NAME test_heltec_sx1262_semtech COMMAND test_heltec_sx1262_semtech)

add_executable(test_heltec_reticulum_radio
    unit/test_heltec_reticulum_radio.c)
target_include_directories(test_heltec_reticulum_radio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_reticulum_radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/heltec_sx1262/include)
target_link_libraries(test_heltec_reticulum_radio PRIVATE reticulum::reticulum)
target_compile_features(test_heltec_reticulum_radio PRIVATE c_std_17)
reticulum_set_project_warnings(test_heltec_reticulum_radio)
add_test(NAME test_heltec_reticulum_radio COMMAND test_heltec_reticulum_radio)

reticulum_add_test(test_crypto unit/test_crypto.c)

# The host shim exercises the complete provider table and libsodium key
# semantics. CI requires this target; local builds without libsodium report an
# explicit skip unless RETICULUM_REQUIRE_ESP_CRYPTO_HOST_TESTS is enabled.
find_path(RETICULUM_SODIUM_INCLUDE_DIR sodium.h)
find_library(RETICULUM_SODIUM_LIBRARY sodium)
if(RETICULUM_SODIUM_INCLUDE_DIR AND RETICULUM_SODIUM_LIBRARY AND
   EXISTS "${RETICULUM_SODIUM_INCLUDE_DIR}/sodium.h" AND
   EXISTS "${RETICULUM_SODIUM_LIBRARY}")
    add_executable(test_esp_crypto
        unit/test_esp_crypto.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf/esp_crypto.c)
    target_include_directories(test_esp_crypto PRIVATE
        ${RETICULUM_SODIUM_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/heltec_wifi_lora_32_v3_1/components/reticulum_idf/include)
    target_link_libraries(test_esp_crypto PRIVATE
        reticulum::reticulum ${RETICULUM_SODIUM_LIBRARY})
    target_compile_features(test_esp_crypto PRIVATE c_std_17)
    reticulum_set_project_warnings(test_esp_crypto)
    add_test(NAME test_esp_crypto COMMAND test_esp_crypto)
elseif(RETICULUM_REQUIRE_ESP_CRYPTO_HOST_TESTS)
    message(FATAL_ERROR
        "ESP crypto host tests are required, but libsodium headers/library were not found")
else()
    message(STATUS
        "ESP crypto host tests disabled: install libsodium or require the gate explicitly")
endif()

# Prove the portable dispatcher links without OpenSSL and fails closed before
# an embedded crypto backend is installed.
add_executable(test_crypto_no_default
    unit/test_crypto_no_default.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/crypto/provider.c)
target_include_directories(test_crypto_no_default PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(test_crypto_no_default PRIVATE c_std_17)
reticulum_set_project_warnings(test_crypto_no_default)
add_test(NAME test_crypto_no_default COMMAND test_crypto_no_default)
reticulum_add_test(test_identity unit/test_identity.c)
reticulum_add_test(test_ratchet_store unit/test_ratchet_store.c)
reticulum_add_test(test_destination unit/test_destination.c)
reticulum_add_test(test_packet unit/test_packet.c)
reticulum_add_test(test_lxmf unit/test_lxmf.c)
reticulum_add_test(test_lxmf_fields unit/test_lxmf_fields.c)
reticulum_add_test(test_lxmf_fields_fuzz unit/test_lxmf_fields_fuzz.c)
target_sources(test_lxmf_fields_fuzz PRIVATE fuzz/fuzz_lxmf_fields.c)
reticulum_add_test(test_lxmf_stamp_job unit/test_lxmf_stamp_job.c)
reticulum_add_test(test_lxmf_paper unit/test_lxmf_paper.c)
reticulum_add_test(test_lxmf_uri_fuzz unit/test_lxmf_uri_fuzz.c)
target_sources(test_lxmf_uri_fuzz PRIVATE fuzz/fuzz_lxmf_uri.c)
reticulum_add_test(test_announce unit/test_announce.c)
reticulum_add_test(test_framing unit/test_framing.c)
reticulum_add_test(test_radio_framing unit/test_radio_framing.c)
reticulum_add_test(test_sx1262_interface unit/test_sx1262_interface.c)
reticulum_add_test(test_auto unit/test_auto.c)
reticulum_add_test(test_transport unit/test_transport.c)
reticulum_add_test(test_reverse_path unit/test_reverse_path.c)
reticulum_add_test(test_link_transport unit/test_link_transport.c)
reticulum_add_test(test_path_store unit/test_path_store.c)
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
reticulum_add_test(test_browser_discovery integration/test_browser_discovery.c)
reticulum_add_test(test_browser_retained integration/test_browser_retained.c)
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
reticulum_add_test(test_tui_interfaces unit/test_tui_interfaces.c)
target_sources(test_tui_interfaces PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_interfaces.c)
target_include_directories(test_tui_interfaces PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_paths unit/test_tui_paths.c)
target_sources(test_tui_paths PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_paths.c)
target_include_directories(test_tui_paths PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps)
reticulum_add_test(test_tui_state unit/test_tui_state.c)
target_sources(test_tui_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_paths.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_interfaces.c
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
reticulum_add_test(test_kiss_serial integration/test_kiss_serial.c)
reticulum_add_test(test_runtime_kiss integration/test_runtime_kiss.c)
reticulum_add_test(test_rnode_serial integration/test_rnode_serial.c)
reticulum_add_test(test_local_instance integration/test_local_instance.c)
reticulum_add_test(test_runtime_local integration/test_runtime_local.c)
reticulum_add_test(test_udp_lxmf integration/test_udp_lxmf.c)
reticulum_add_test(test_runtime_receipt integration/test_runtime_receipt.c)
reticulum_add_test(test_runtime_path_response integration/test_runtime_path_response.c)
reticulum_add_test(test_lxmf_router_receipt integration/test_lxmf_router_receipt.c)
reticulum_add_test(test_lxmf_router_recovery unit/test_lxmf_router_recovery.c)
reticulum_add_test(test_lxmf_identity_discovery integration/test_lxmf_identity_discovery.c)
reticulum_add_test(test_lxmf_router_direct integration/test_lxmf_router_direct.c)
reticulum_add_test(test_runtime_inbound_link integration/test_runtime_inbound_link.c)
reticulum_add_test(test_runtime_link_transport integration/test_runtime_link_transport.c)
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
    reticulum_add_test(test_tui_qr unit/test_tui_qr.c)
    target_link_libraries(test_tui_qr PRIVATE nomad_qr)
    add_executable(test_tui_settings_ui
        unit/test_tui_settings_ui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_interfaces.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_paths.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
    target_include_directories(test_tui_settings_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps ${CURSES_INCLUDE_DIRS})
    target_link_libraries(test_tui_settings_ui PRIVATE
        reticulum::reticulum nomad_qr ${CURSES_LIBRARIES})
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_interfaces.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_paths.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
    target_include_directories(test_tui_dispatch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps ${CURSES_INCLUDE_DIRS})
    target_link_libraries(test_tui_dispatch PRIVATE
        reticulum::reticulum nomad_qr ${CURSES_LIBRARIES})
    target_compile_features(test_tui_dispatch PRIVATE c_std_17)
    reticulum_set_project_warnings(test_tui_dispatch)
    add_test(NAME test_tui_dispatch COMMAND test_tui_dispatch)

    add_executable(test_tui_fields_ui
        unit/test_tui_fields_ui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_interfaces.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_paths.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_rrc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_settings.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_editor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/tui_text.c)
    target_include_directories(test_tui_fields_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps ${CURSES_INCLUDE_DIRS})
    target_link_libraries(test_tui_fields_ui PRIVATE
        reticulum::reticulum nomad_qr ${CURSES_LIBRARIES})
    target_compile_features(test_tui_fields_ui PRIVATE c_std_17)
    reticulum_set_project_warnings(test_tui_fields_ui)
    add_test(NAME test_tui_fields_ui COMMAND test_tui_fields_ui)
endif()
