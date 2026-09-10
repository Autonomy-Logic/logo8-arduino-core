# CMake toolchain file: bare-metal Cortex-M4F, using the SAME arm-none-eabi-gcc
# the Arduino core ships (Energia 8.3.1-20190703).
#
# Using a newer compiler here would measure a toolchain we do not ship, and the
# footprint numbers this build exists to produce would not be the ones users get.
#
# The ABI flags mirror platform.txt exactly.  They are not cosmetic: an archive
# built with a different -mfloat-abi or -mfpu will not link against the sketch
# at all, and a different -O level would misreport the size.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# CMake runs this file again inside its own try_compile sub-project, and a
# -D on the outer command line does NOT reach that sub-project.  Without the
# PLATFORM_VARIABLES line below the second pass sees an empty
# ARM_TOOLCHAIN_BIN and configure dies with "CMAKE_C_COMPILER not set" — an
# error that points at the compiler rather than at the missing variable.
# The env fallback covers invoking cmake by hand.
if(NOT ARM_TOOLCHAIN_BIN AND DEFINED ENV{ARM_TOOLCHAIN_BIN})
  set(ARM_TOOLCHAIN_BIN "$ENV{ARM_TOOLCHAIN_BIN}")
endif()
if(NOT ARM_TOOLCHAIN_BIN)
  message(FATAL_ERROR "ARM_TOOLCHAIN_BIN must point at the core's arm-none-eabi bin/ directory")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARM_TOOLCHAIN_BIN)

set(CMAKE_C_COMPILER   "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-g++")
set(CMAKE_ASM_COMPILER "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-gcc")
set(CMAKE_AR           "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-ar"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY      "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE         "${ARM_TOOLCHAIN_BIN}/arm-none-eabi-size"    CACHE FILEPATH "")

# There is no libc startup or linker script here, so CMake's default
# "compile AND link a test executable" probe cannot succeed.  Building the probe
# as a static library is the supported way to say "this is a freestanding
# target" — without it configure fails before it has compiled a single source.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mcpu / -mthumb / -mfloat-abi / -mfpu: from platform.txt, must match the sketch.
# -Os / -ffunction-sections / -fdata-sections: also from platform.txt, so the
#   archive is subject to the same --gc-sections the final link performs.
set(ARM_ABI_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(ARM_OPT_FLAGS "-Os -ffunction-sections -fdata-sections")

# The shim include path lives here rather than being passed as
# -DCMAKE_C_FLAGS by the caller. That is not a style preference: CMAKE_C_FLAGS
# is a cache variable and CMAKE_C_FLAGS_INIT only SEEDS it, so a
# -DCMAKE_C_FLAGS on the command line silently replaces every flag below.
# Doing that produced an archive with no -mcpu/-mthumb/-mfloat-abi at all —
# soft-float, wrong ISA — which still built cleanly and only failed much later
# at link ("uses VFP register arguments, ... does not"). Any size measured from
# such an archive is meaningless, so the flags stay in one place.
set(ARM_SHIM_INCLUDE "-I${CMAKE_CURRENT_LIST_DIR}/shim")

set(CMAKE_C_FLAGS_INIT   "${ARM_ABI_FLAGS} ${ARM_OPT_FLAGS} ${ARM_SHIM_INCLUDE}")
set(CMAKE_CXX_FLAGS_INIT "${ARM_ABI_FLAGS} ${ARM_OPT_FLAGS} ${ARM_SHIM_INCLUDE}")
set(CMAKE_ASM_FLAGS_INIT "${ARM_ABI_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
