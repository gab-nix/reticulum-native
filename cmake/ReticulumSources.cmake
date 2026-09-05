# Explicit source manifests keep platform-specific code out of embedded builds.
# Paths are relative to the repository root so the same lists can be consumed by
# the host CMake project and ESP-IDF component wrappers.

set(RETICULUM_PORTABLE_SOURCES
    src/announce/announce.c
    src/crypto/provider.c
    src/destination/destination.c
    src/identity/identity.c
    src/interfaces/auto.c
    src/interfaces/framing.c
    src/interfaces/radio_framing.c
    src/interfaces/sx1262_interface.c
    src/link/channel.c
    src/link/link.c
    src/link/request.c
    src/link/resource.c
    src/lxmf/delivery.c
    src/lxmf/fields.c
    src/lxmf/lxmf.c
    src/lxmf/micron.c
    src/lxmf/paper.c
    src/lxmf/propagation.c
    src/lxmf/propagation_session.c
    src/lxmf/router.c
    src/lxmf/router_send.c
    src/nomad/browser.c
    src/nomad/hosted_form.c
    src/nomad/rrc.c
    src/nomad/rrc_session.c
    src/packet/packet.c
    src/platform/buffer.c
    src/platform/interface.c
    src/platform/platform.c
    src/platform/status.c
    src/platform/storage.c
    src/platform/storage_record.c
    src/transport/ifac.c
    src/transport/node.c
    src/transport/path_store.c
    src/transport/proof.c
    src/transport/transport.c)

# These modules currently use POSIX files or other host persistence directly.
# They remain separate until the storage-provider conversion is complete.
set(RETICULUM_HOST_STORAGE_SOURCES
    src/config/config.c
    src/identity/ratchet_store.c
    src/lxmf/peer_store.c
    src/lxmf/store.c
    src/lxmf/tickets.c
    src/nomad/hosted_node.c
    src/transport/node_registry.c)

# Socket and serial implementations are never included in an ESP-IDF target.
set(RETICULUM_POSIX_INTERFACE_SOURCES
    src/interfaces/auto_posix.c
    src/interfaces/kiss.c
    src/interfaces/local.c
    src/interfaces/rnode.c
    src/interfaces/tcp.c
    src/interfaces/udp.c
    src/platform/posix_hal.c
    src/runtime/runtime.c)

# Only the host crypto provider depends on OpenSSL.
set(RETICULUM_OPENSSL_SOURCES
    src/crypto/crypto.c)

set(RETICULUM_HOST_SOURCES
    ${RETICULUM_PORTABLE_SOURCES}
    ${RETICULUM_HOST_STORAGE_SOURCES}
    ${RETICULUM_POSIX_INTERFACE_SOURCES}
    ${RETICULUM_OPENSSL_SOURCES})

function(reticulum_validate_source_manifest)
    set(_seen)
    foreach(_source IN LISTS RETICULUM_HOST_SOURCES)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
            message(FATAL_ERROR "Reticulum source manifest entry does not exist: ${_source}")
        endif()
        if(_source IN_LIST _seen)
            message(FATAL_ERROR "Duplicate Reticulum source manifest entry: ${_source}")
        endif()
        list(APPEND _seen "${_source}")
    endforeach()

    file(GLOB_RECURSE _actual_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
    list(SORT _actual_sources)
    set(_manifest_sources ${RETICULUM_HOST_SOURCES})
    list(SORT _manifest_sources)
    if(NOT _actual_sources STREQUAL _manifest_sources)
        message(FATAL_ERROR
            "Reticulum source manifest is incomplete.\n"
            "Files under src: ${_actual_sources}\n"
            "Manifest files: ${_manifest_sources}")
    endif()
endfunction()
