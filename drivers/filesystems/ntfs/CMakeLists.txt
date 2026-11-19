
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers
                    inc)

include(${REACTOS_SOURCE_DIR}/sdk/cmake/VerosBuildenv.cmake)
set(NTFS_RUST_LIB "${CMAKE_CURRENT_BINARY_DIR}/rust/x86_64-pc-windows-gnu/release/libntfs_km.a")
set(NTFS_CARGO "${VEROS_CHECKOUT}/tools/rust/cargo/bin/cargo.exe")
set(NTFS_GCC "${VEROS_CHECKOUT}/tools/rosbe/amd64/bin/x86_64-w64-mingw32-gcc.exe")
set(NTFS_GXX "${VEROS_CHECKOUT}/tools/rosbe/amd64/bin/x86_64-w64-mingw32-g++.exe")
set(NTFS_RUST_LINKER "${VEROS_CHECKOUT}/tools/rust/rustup/toolchains/1.99.1-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained/x86_64-w64-mingw32-gcc.exe")

add_custom_target(ntfs_rust
    COMMAND ${CMAKE_COMMAND} -E env
        RUSTUP_HOME=${VEROS_CHECKOUT}/tools/rust/rustup
        CARGO_HOME=${VEROS_CHECKOUT}/tools/rust/cargo
        RUSTUP_TOOLCHAIN=1.99.1-x86_64-pc-windows-gnu
        CC=${NTFS_GCC}
        CXX=${NTFS_GXX}
        CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=${NTFS_RUST_LINKER}
        RUSTFLAGS=-C\ link-self-contained=yes
        ${NTFS_CARGO} build --release --target x86_64-pc-windows-gnu
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
            --target-dir ${CMAKE_CURRENT_BINARY_DIR}/rust
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
    BYPRODUCTS "${NTFS_RUST_LIB}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM)

list(APPEND SOURCE
    attrib.c
    blockdev.c
    btree.c
    cleanup.c
    close.c
    create.c
    devctl.c
    dirctl.c
    dispatch.c
    fastio.c
    flush.c
    fcb.c
    finfo.c
    fsctl.c
    lockctrl.c
    mft.c
    misc.c
    ntfs3x.c
    ntfs.c
    rw.c
    volinfo.c
    ntfs.h)

add_library(ntfs MODULE ${SOURCE} ntfs.rc)

add_definitions(-D__KERNEL__)
set_module_type(ntfs kernelmodedriver)
target_link_libraries(ntfs ${PSEH_LIB} "${NTFS_RUST_LIB}")
add_dependencies(ntfs ntfs_rust)
add_importlibs(ntfs ntoskrnl hal)
add_cd_file(TARGET ntfs DESTINATION VerOS/system32/drivers NO_CAB FOR all)
