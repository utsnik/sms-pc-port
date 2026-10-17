set(SMS_WIIU_BUILD_DIR "/mnt/buildcache/wiiu-build/sms" CACHE PATH
    "Stage 1 Wii U output directory")

set(SMS_WIIU_GAME_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/port_include
  ${SMS_PATCHED_INCLUDE_DIRS}
  ${SMS_GAME_INCLUDE_DIRS})

# The game and wut both expose symbols named OSCreateThread, OSJoinThread,
# OSMutex, and OSMessageQueue, but their object layouts and ABIs differ.  The
# decomp-facing names are redirected to the adapters in platform/wiiu; those
# adapters call wut through explicit asm-labelled declarations.
set(SMS_WIIU_OS_ADAPTER_DEFS
  OSInitThreadQueue=sms_wiiu_OSInitThreadQueue
  OSGetCurrentThread=sms_wiiu_OSGetCurrentThread
  OSIsThreadTerminated=sms_wiiu_OSIsThreadTerminated
  OSDisableScheduler=sms_wiiu_OSDisableScheduler
  OSEnableScheduler=sms_wiiu_OSEnableScheduler
  OSYieldThread=sms_wiiu_OSYieldThread
  OSCreateThread=sms_wiiu_OSCreateThread
  OSExitThread=sms_wiiu_OSExitThread
  OSCancelThread=sms_wiiu_OSCancelThread
  OSJoinThread=sms_wiiu_OSJoinThread
  OSDetachThread=sms_wiiu_OSDetachThread
  OSResumeThread=sms_wiiu_OSResumeThread
  OSSuspendThread=sms_wiiu_OSSuspendThread
  OSSleepThread=sms_wiiu_OSSleepThread
  OSWakeupThread=sms_wiiu_OSWakeupThread
  OSGetThreadPriority=sms_wiiu_OSGetThreadPriority
  OSDisableInterrupts=sms_wiiu_OSDisableInterrupts
  OSEnableInterrupts=sms_wiiu_OSEnableInterrupts
  OSRestoreInterrupts=sms_wiiu_OSRestoreInterrupts
  OSGetTime=sms_wiiu_OSGetTime
  OSGetTick=sms_wiiu_OSGetTick
  OSInitMessageQueue=sms_wiiu_OSInitMessageQueue
  OSSendMessage=sms_wiiu_OSSendMessage
  OSJamMessage=sms_wiiu_OSJamMessage
  OSReceiveMessage=sms_wiiu_OSReceiveMessage
  OSInitMutex=sms_wiiu_OSInitMutex
  OSLockMutex=sms_wiiu_OSLockMutex
  OSUnlockMutex=sms_wiiu_OSUnlockMutex
  OSTryLockMutex=sms_wiiu_OSTryLockMutex
  OSInitCond=sms_wiiu_OSInitCond
  OSWaitCond=sms_wiiu_OSWaitCond
  OSSignalCond=sms_wiiu_OSSignalCond)

set(SMS_WIIU_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/ar/ar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/noaudio.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_dispatch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_game.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_j3d.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_jaudio.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_jpa.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/endian/endian_seq.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/endian.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/msl_math.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/msl_rand.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/sdk_data.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/mtx/mtx.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/sdk_stubs.cpp)

file(GLOB SMS_WIIU_PS_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/ps/*.S)

add_library(sms_game STATIC
  ${SMS_DECOMP_CXX_SOURCES}
  ${SMS_DECOMP_C_SOURCES}
  ${SMS_DECOMP}/libs/dolphin/src/pad/Padclamp.c)
set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(sms_game BEFORE PRIVATE ${SMS_WIIU_GAME_INCLUDES})
target_compile_definitions(sms_game PRIVATE ${SMS_COMMON_DEFS})
target_compile_options(sms_game PRIVATE
  ${SMS_GAME_FLAGS}
  -mcpu=750 -meabi -mhard-float -ffunction-sections -fdata-sections)
set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES}
  PROPERTIES OBJECT_DEPENDS ${SMS_PATCH_HEADER_STAMP})
set_source_files_properties(${SMS_DECOMP_PCH_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${SMS_GAME_PCH_DIR}/SMS.pch")

add_executable(sms
  ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/os_core.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/os_thread.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dvd.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/vi.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/exception.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pad.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gx_stub.cpp
  ${SMS_WIIU_COMMON_SOURCES}
  ${SMS_WIIU_PS_SOURCES})
target_include_directories(sms BEFORE PRIVATE ${SMS_WIIU_GAME_INCLUDES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(sms PRIVATE ${SMS_COMMON_DEFS} ${SMS_WIIU_OS_ADAPTER_DEFS}
  SMS_AUDIO_DSP_HLE=0 SMS_WIIU_RUN_GAME=1)
target_compile_definitions(sms_game PRIVATE ${SMS_WIIU_OS_ADAPTER_DEFS})
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/os_core.cpp
  PROPERTIES COMPILE_DEFINITIONS DEBUG=1)
set_source_files_properties(${SMS_DECOMP}/libs/JSystem/src/JKernel/JKRHeap.cpp
  PROPERTIES COMPILE_DEFINITIONS DEBUG=1
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/debug_os.h")
target_compile_options(sms PRIVATE
  -mcpu=750 -meabi -mhard-float -fno-strict-aliasing -fwrapv
  -ffunction-sections -fdata-sections -fno-omit-frame-pointer
  -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -std=gnu++11)

set_target_properties(sms PROPERTIES
  OUTPUT_NAME sms
  SUFFIX ".elf"
  RUNTIME_OUTPUT_DIRECTORY ${SMS_WIIU_BUILD_DIR})
target_link_options(sms PRIVATE
  "-Wl,-Map,${SMS_WIIU_BUILD_DIR}/sms.map"
  -Wl,--gc-sections)
target_link_libraries(sms PRIVATE
  -Wl,--whole-archive sms_game -Wl,--no-whole-archive
  wut m)

add_custom_target(sms_wiiu_elf ALL DEPENDS sms)
