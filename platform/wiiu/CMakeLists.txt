set(SMS_WIIU_BUILD_DIR "/mnt/buildcache/wiiu-build/sms" CACHE PATH
    "Stage 1 Wii U output directory")
option(SMS_WIIU_SKIP_INTRO "Skip opening movies in Wii U test builds" ON)

set(SMS_WIIU_GAME_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/include
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

# The game's GameCube AI/DSP API shares names with the Wii U's own sndcore2
# exports, and wut's sndcore2 import stubs are linked once SDL2 (audio.cpp)
# uses AX. The decomp-facing names are renamed for the game and the port, so
# sndcore2's definitions stay its own.
set(SMS_WIIU_SND_ADAPTER_DEFS
  AIInit=sms_gc_AIInit
  AICheckInit=sms_gc_AICheckInit
  AIReset=sms_gc_AIReset
  AIRegisterDMACallback=sms_gc_AIRegisterDMACallback
  AIInitDMA=sms_gc_AIInitDMA
  AIGetDMAEnableFlag=sms_gc_AIGetDMAEnableFlag
  AIStartDMA=sms_gc_AIStartDMA
  AIStopDMA=sms_gc_AIStopDMA
  AIGetDMABytesLeft=sms_gc_AIGetDMABytesLeft
  AIGetDMAStartAddr=sms_gc_AIGetDMAStartAddr
  AIGetDMALength=sms_gc_AIGetDMALength
  AISetDSPSampleRate=sms_gc_AISetDSPSampleRate
  AIGetDSPSampleRate=sms_gc_AIGetDSPSampleRate
  DSPInit=sms_gc_DSPInit
  DSPCheckInit=sms_gc_DSPCheckInit
  DSPReset=sms_gc_DSPReset
  DSPHalt=sms_gc_DSPHalt
  DSPUnhalt=sms_gc_DSPUnhalt
  DSPAddTask=sms_gc_DSPAddTask
  DSPCancelTask=sms_gc_DSPCancelTask
  DSPAssertTask=sms_gc_DSPAssertTask
  DSPAssertInt=sms_gc_DSPAssertInt
  DSPGetDMAStatus=sms_gc_DSPGetDMAStatus
  DSPCheckMailFromDSP=sms_gc_DSPCheckMailFromDSP
  DSPCheckMailToDSP=sms_gc_DSPCheckMailToDSP
  DSPReadMailFromDSP=sms_gc_DSPReadMailFromDSP
  DSPSendMailToDSP=sms_gc_DSPSendMailToDSP
  DSPQuit=sms_gc_DSPQuit
)

set(SMS_WIIU_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/ar/ar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/dsp_hle.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/dsp_mixer.cpp
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/card/card.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/mtx/mtx.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/sdk_stubs.cpp)

file(GLOB SMS_WIIU_PS_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/ps/*.S)

# GD display-list builders (J3DShape::makeVcdVatCmd and friends): compiled from
# the decompiled SDK like the PC build's platform/gx; the sdk_stubs versions left
# the list uninitialised and the first command write went to address 0.
set(SMS_WIIU_GD_SOURCES
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDBase.c
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDGeometry.c
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDLight.c
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDPixel.c
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDTev.c
  ${SMS_DECOMP}/libs/dolphin/src/gd/GDTransform.c)
set_source_files_properties(${SMS_WIIU_GD_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/gd_wiiu_prefix.h;-w;-fcommon")

add_library(sms_game STATIC
  ${SMS_DECOMP_CXX_SOURCES}
  ${SMS_DECOMP_C_SOURCES}
  ${SMS_DECOMP}/libs/dolphin/src/pad/Padclamp.c
  ${SMS_WIIU_GD_SOURCES})
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
  ${CMAKE_CURRENT_SOURCE_DIR}/profiler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_sdl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gx_stub.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src/gx_api.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src/gx_fifo.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src/gx_vert.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src/gx_texture_pack_common.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gx2_texture_pack.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gx2_texture.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gx2_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/port_heap.cpp
  ${SMS_WIIU_COMMON_SOURCES}
  ${SMS_WIIU_PS_SOURCES})
target_include_directories(sms BEFORE PRIVATE ${SMS_WIIU_GAME_INCLUDES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(sms PRIVATE ${SMS_COMMON_DEFS} ${SMS_WIIU_OS_ADAPTER_DEFS} ${SMS_WIIU_SND_ADAPTER_DEFS}
  SMS_AUDIO_DSP_HLE=1 SMS_WIIU_RUN_GAME=1
  SMS_WIIU_SKIP_INTRO=$<BOOL:${SMS_WIIU_SKIP_INTRO}>)
target_compile_definitions(sms_game PRIVATE ${SMS_WIIU_OS_ADAPTER_DEFS} ${SMS_WIIU_SND_ADAPTER_DEFS})
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/os_core.cpp
  PROPERTIES COMPILE_DEFINITIONS DEBUG=1)
set_source_files_properties(${SMS_DECOMP}/libs/JSystem/src/JKernel/JKRHeap.cpp
  PROPERTIES COMPILE_DEFINITIONS DEBUG=1
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/debug_os.h")
target_compile_options(sms PRIVATE
  -mcpu=750 -meabi -mhard-float -fno-strict-aliasing -fwrapv
  -ffunction-sections -fdata-sections ${SMS_WIIU_FP_FLAG}
  ${SMS_WIIU_PORT_FMA_FLAG}
  -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -std=gnu++11)

set_target_properties(sms PROPERTIES
  OUTPUT_NAME sms
  SUFFIX ".elf"
  RUNTIME_OUTPUT_DIRECTORY ${SMS_WIIU_BUILD_DIR})
target_link_options(sms PRIVATE
  "-Wl,-Map,${SMS_WIIU_BUILD_DIR}/sms.map"
  -Wl,--gc-sections
  -Wl,--wrap=_Znwj -Wl,--wrap=_Znaj -Wl,--wrap=_ZdlPv -Wl,--wrap=_ZdaPv -Wl,--wrap=_ZdlPvj
  -Wl,--wrap=DCFlushRange -Wl,--wrap=DCStoreRange -Wl,--wrap=DCFlushRangeNoSync -Wl,--wrap=DCStoreRangeNoSync
  -Wl,--wrap=memset)
# Audio output (audio.cpp): wut's SDL2 port, audio subsystem only.
find_package(SDL2 REQUIRED CONFIG)
target_link_libraries(sms PRIVATE
  -Wl,--whole-archive sms_game -Wl,--no-whole-archive
  SDL2::SDL2-static wut m)

add_custom_target(sms_wiiu_elf ALL DEPENDS sms)
