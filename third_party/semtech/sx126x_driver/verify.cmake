function(reticulum_verify_semtech_sx126x root)
    set(files
        LICENSE.txt
        src/sx126x.c
        src/sx126x.h
        src/sx126x_driver_version.c
        src/sx126x_driver_version.h
        src/sx126x_hal.h
        src/sx126x_regs.h
        src/sx126x_status.h)
    set(expected
        a158fe2180a1429e59d7552ca3c31e4b312aca958423f3e98c15bb63fcb05ea9
        364465db57f5ebaf216934dc45676ff83a8cdee162a1d1b5b6da8cea61dfa4ca
        798e0aa7d773992371d82f747ebe66e00c6c0ec54b3b3a0f528822828e0d063f
        c4bf85aac4e36a36d854edbf1ccd2535bfcf41a479d67afdde2fe176b2851ea0
        25f2fb12dfb5b0c1b0f9116d962abd00f5a961c8235d66d3d8a0920818c75e5f
        b70a5de4265a0074ab3c8577a4fd55f10634e02351fabc5c16f752cccc102d2c
        dd64a28905140a9d6d21bc91c18db4fa039e24503d8534e333bf48300a3dfb28
        043780bfeecdfb9d22137cbac33521aa2c40d052e3338c74ff0ff86feb176235)

    list(LENGTH files file_count)
    math(EXPR last_index "${file_count} - 1")
    foreach(index RANGE ${last_index})
        list(GET files ${index} relative_path)
        list(GET expected ${index} expected_sha256)
        set(path "${root}/${relative_path}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Missing pinned Semtech SX126x file: ${relative_path}")
        endif()
        file(SHA256 "${path}" actual_sha256)
        if(NOT actual_sha256 STREQUAL expected_sha256)
            message(FATAL_ERROR
                "Pinned Semtech SX126x checksum mismatch for ${relative_path}: "
                "expected ${expected_sha256}, got ${actual_sha256}")
        endif()
    endforeach()
endfunction()
