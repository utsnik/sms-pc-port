# THP decoders: the decomp's own SDK decoder (libs/dolphin/src/thp/THPDec.c,
# THPAudio.c) with decomp-patches/thp-0*.patch applied, compiled into the
# selected game target. Their strong THPInit/THPVideoDecode/THPAudioDecode
# replace the weak SDK stubs in the port boundary.
#
# Use either:
#   include(${CMAKE_CURRENT_SOURCE_DIR}/platform/thp/thp.cmake)   # after add_library(sms_game ...)
# or, without editing CMakeLists.txt:
#   cmake -DCMAKE_PROJECT_INCLUDE=<repo>/platform/thp/thp.cmake ...
function(sms_thp_add_decoders target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "platform/thp/thp.cmake: ${target} does not exist yet")
  endif()
  set(_thp ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPDec.c ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPAudio.c)
  foreach(f ${_thp})
    if(NOT EXISTS ${f})
      message(FATAL_ERROR "platform/thp: ${f} missing (are decomp-patches/thp-*.patch applied?)")
    endif()
  endforeach()
  target_sources(${target} PRIVATE ${_thp})
  message(STATUS "THP decoders linked into ${target}")
endfunction()

function(sms_thp_add_default_decoders)
  if(TARGET sms_game)
    sms_thp_add_decoders(sms_game)
  elseif(TARGET smg_game)
    sms_thp_add_decoders(smg_game)
  else()
    message(FATAL_ERROR "platform/thp/thp.cmake: no game target exists yet")
  endif()
endfunction()

if(TARGET sms_game OR TARGET smg_game)
  sms_thp_add_default_decoders()
else()
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL sms_thp_add_default_decoders)
endif()
