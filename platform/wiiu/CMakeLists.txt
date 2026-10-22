if(PORT_GAME STREQUAL "smg")
  set(SMG_WIIU_BUILD_DIR "${PORT_WIIU_BUILD_DIR}" CACHE PATH
      "Galaxy Stage 1 Wii U output directory")

  set(SMG_WIIU_OS_ADAPTER_DEFS
    OSInitThreadQueue=sms_wiiu_OSInitThreadQueue
    OSGetCurrentThread=sms_wiiu_OSGetCurrentThread
    OSIsThreadTerminated=sms_wiiu_OSIsThreadTerminated
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
    OSDisableScheduler=sms_wiiu_OSDisableScheduler
    OSEnableScheduler=sms_wiiu_OSEnableScheduler
    OSYieldThread=sms_wiiu_OSYieldThread
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
    OSTryLockMutex=sms_wiiu_OSTryLockMutex)
  set(SMG_WIIU_SND_ADAPTER_DEFS
    AIInit=smg_gc_AIInit
    AICheckInit=smg_gc_AICheckInit
    AIReset=smg_gc_AIReset
    AIRegisterDMACallback=smg_gc_AIRegisterDMACallback
    AIInitDMA=smg_gc_AIInitDMA
    AIGetDMAEnableFlag=smg_gc_AIGetDMAEnableFlag
    AIStartDMA=smg_gc_AIStartDMA
    AIStopDMA=smg_gc_AIStopDMA
    AIGetDMABytesLeft=smg_gc_AIGetDMABytesLeft
    DSPInit=smg_gc_DSPInit
    DSPCheckInit=smg_gc_DSPCheckInit
    DSPReset=smg_gc_DSPReset
    DSPHalt=smg_gc_DSPHalt
    DSPUnhalt=smg_gc_DSPUnhalt
    DSPAddTask=smg_gc_DSPAddTask)

  # Keep the legacy shared-provider candidates explicit for the ownership
  # audit. Stage 1 deliberately does not compile these audio/runtime sources:
  # the Galaxy boundary supplies neutral no-data hooks instead.
  set(SMG_WIIU_UNSELECTED_PROVIDER_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/ai.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/dsp_hle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/dsp_mixer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/audio/noaudio.cpp)
  set(SMG_WIIU_RUNTIME_FILE runtime.cpp)

  # Shared Sunshine providers are compiled once here. Do not add their
  # Petari counterparts to SMG_GAME_SOURCES to resolve a missing symbol.
  set(SMG_WIIU_SHARED_PROVIDER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/gx/src/gx_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/gx/src/gx_fifo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/gx/src/gx_vert.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/os_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/os_thread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/vi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/dvd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/pad.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/ar/ar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/mtx/mtx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/wiiu/port_heap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/msl_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/misc/msl_rand.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDBase.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDGeometry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDLight.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDPixel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDTev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/src/gd/GDTransform.c)
  set(SMG_WIIU_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/sdk_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/nand.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/${SMG_WIIU_RUNTIME_FILE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/dol_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/galaxy_link_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/gx_link_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/ps/ps_probe.S)

  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/platform/wiiu/ps/ps_probe.S
    PROPERTIES COMPILE_OPTIONS "-Wa,-mgekko")

  # Sunshine's providers use the Sunshine Dolphin/JSystem ABI. Compile them
  # as a separate object target so Galaxy's revolution headers and forced
  # compatibility shim cannot be mixed into the same translation unit.
  set(SMG_WIIU_SHARED_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/dolphin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/JSystem/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/PowerPC_EABI_Support/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/TRK_MINNOW_DOLPHIN/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../decomp/libs/OdemuExi2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/port_include)
  add_library(smg_wiiu_shared OBJECT ${SMG_WIIU_SHARED_PROVIDER_SOURCES})
  target_include_directories(smg_wiiu_shared BEFORE PRIVATE ${SMG_WIIU_SHARED_INCLUDE_DIRS})
  target_compile_definitions(smg_wiiu_shared PRIVATE
    GEKKO=1 TARGET_PC=1 TARGET_WIIU=1 __WIIU__=1 ESPRESSO=1 NDEBUG=1 DEBUG=1 VERSION=0
    SMS_AUDIO_DSP_HLE=1 PORT_GAME_SMG=1
    ${SMG_WIIU_OS_ADAPTER_DEFS} ${SMG_WIIU_SND_ADAPTER_DEFS})
  target_compile_options(smg_wiiu_shared PRIVATE
    ${SMG_GAME_FLAGS} -std=gnu++11
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../src/port_compat.h
    -include${CMAKE_CURRENT_SOURCE_DIR}/dolphin_externs.h)

  add_library(smg_game STATIC ${SMG_GAME_SOURCES})
  target_include_directories(smg_game BEFORE PRIVATE
    ${SMG_GAME_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
  target_compile_definitions(smg_game PRIVATE ${SMG_DEFS}
    ${SMG_WIIU_OS_ADAPTER_DEFS} ${SMG_WIIU_SND_ADAPTER_DEFS})
  target_compile_options(smg_game PRIVATE ${SMG_GAME_FLAGS}
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/include/port/compat.h)

  add_executable(smg ${SMG_WIIU_PLATFORM_SOURCES}
    $<TARGET_OBJECTS:smg_wiiu_shared>)
  target_include_directories(smg BEFORE PRIVATE
    ${SMG_GAME_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
  target_compile_definitions(smg PRIVATE ${SMG_DEFS}
    ${SMG_WIIU_OS_ADAPTER_DEFS} ${SMG_WIIU_SND_ADAPTER_DEFS})
  target_compile_options(smg PRIVATE ${SMG_GAME_FLAGS}
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../games/smg/include/port/compat.h)
  set_target_properties(smg PROPERTIES
    OUTPUT_NAME smg
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY ${SMG_WIIU_BUILD_DIR})
  target_link_options(smg PRIVATE
    -Wl,--no-undefined
    -Wl,--gc-sections
    "-Wl,-Map,${SMG_WIIU_BUILD_DIR}/smg.map")
  target_link_libraries(smg PRIVATE
    -Wl,--whole-archive smg_game -Wl,--no-whole-archive
    wut m)
  add_custom_target(smg_wiiu_elf ALL DEPENDS smg)
  return()
endif()

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
