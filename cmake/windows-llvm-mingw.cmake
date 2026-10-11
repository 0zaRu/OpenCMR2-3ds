# Cross-build Windows x64 from Linux with the official LLVM-MinGW bundle.
# Set OPENCMR2_LLVM_MINGW to the extracted bundle; SDL3_DIR may point to an
# existing Windows SDL3 package. Host shader tools must remain host programs.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT LLVM_MINGW_ROOT)
    if(NOT DEFINED ENV{OPENCMR2_LLVM_MINGW})
        message(FATAL_ERROR "Set OPENCMR2_LLVM_MINGW to the extracted LLVM-MinGW toolchain")
    endif()
    file(TO_CMAKE_PATH "$ENV{OPENCMR2_LLVM_MINGW}" LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "LLVM-MinGW toolchain directory")
endif()
set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
