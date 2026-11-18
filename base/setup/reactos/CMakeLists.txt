
list(APPEND SOURCE
    spapisup/fileqsup.c
    spapisup/infsupp.c
    drivepage.c
    reactos.c
    stage.c
    treelist.c
    reactos.h)

file(GLOB reactos_rc_deps res/*.*)
add_rc_deps(reactos.rc ${reactos_rc_deps})
add_executable(reactos ${SOURCE} reactos.rc)
set_target_properties(reactos PROPERTIES OUTPUT_NAME "Tavera OS")
add_pch(reactos reactos.h SOURCE)

set_module_type(reactos win32gui UNICODE)
target_link_libraries(reactos uuid)
target_link_libraries(reactos zlib_solo) ## We use USETUP's cabinet implementation
add_delay_importlibs(reactos setuplib)
add_importlibs(reactos advapi32 gdi32 user32 comctl32 shlwapi setupapi msvcrt kernel32 ntdll)
add_cd_file(TARGET reactos DESTINATION "Tavera" NO_CAB FOR bootcd)
