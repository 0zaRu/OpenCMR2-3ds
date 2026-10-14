# The installable CIA of the Nintendo 3DS build (src/CMakeLists.txt), made
# from opencmr2.elf with bannertool (banner and icon) and makerom. Both are
# looked for on the PATH or in OPENCMR2_3DS_TOOLS; without them only the
# .3dsx is built (see docs/3DS.md).

set(OPENCMR2_3DS_TOOLS "" CACHE PATH "Directory with makerom and bannertool for the 3DS CIA")
find_program(CTR_MAKEROM_EXE makerom HINTS ${OPENCMR2_3DS_TOOLS} "${DEVKITPRO}/tools/bin")
find_program(CTR_BANNERTOOL_EXE bannertool HINTS ${OPENCMR2_3DS_TOOLS} "${DEVKITPRO}/tools/bin")

if(NOT CTR_MAKEROM_EXE OR NOT CTR_BANNERTOOL_EXE)
    message(STATUS "OpenCMR2: makerom or bannertool not found, building the .3dsx without the CIA")
    return()
endif()

set(CIA_DIR ${CMAKE_CURRENT_BINARY_DIR}/cia)
set(CIA_BANNER ${CIA_DIR}/banner.bnr)
set(CIA_ICON ${CIA_DIR}/icon.icn)
set(CIA_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/OpenCMR2.cia)

add_custom_command(
    OUTPUT ${CIA_BANNER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CIA_DIR}
    COMMAND ${CTR_BANNERTOOL_EXE} makebanner -i ${CTR_RES}/banner.png -a ${CTR_RES}/banner.wav -o ${CIA_BANNER}
    DEPENDS ${CTR_RES}/banner.png ${CTR_RES}/banner.wav
    VERBATIM
    COMMENT "Making the CIA banner")
add_custom_command(
    OUTPUT ${CIA_ICON}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CIA_DIR}
    COMMAND ${CTR_BANNERTOOL_EXE} makesmdh -s "OpenCMR2" -l "Colin McRae Rally 2.0 (needs the PC game files)"
            -p "OpenCMR2" -i ${CTR_RES}/icon.png -o ${CIA_ICON}
    DEPENDS ${CTR_RES}/icon.png
    VERBATIM
    COMMENT "Making the CIA icon")
add_custom_command(
    OUTPUT ${CIA_OUTPUT}
    COMMAND ${CTR_MAKEROM_EXE} -f cia -o ${CIA_OUTPUT} -target t -exefslogo
            -elf $<TARGET_FILE:opencmr2> -rsf ${CTR_RES}/opencmr2.rsf -icon ${CIA_ICON} -banner ${CIA_BANNER}
    DEPENDS opencmr2 ${CIA_BANNER} ${CIA_ICON} ${CTR_RES}/opencmr2.rsf
    VERBATIM
    COMMENT "Making OpenCMR2.cia")
add_custom_target(opencmr2_cia ALL DEPENDS ${CIA_OUTPUT})
