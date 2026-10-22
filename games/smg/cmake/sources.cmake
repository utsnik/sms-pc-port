# Stage 1 Galaxy source ownership manifest.
#
# This file is intentionally explicit about the only Petari SDK translation
# units that may enter smg_game. Shared SDK providers are selected by the
# Sunshine Wii U platform target, not by a broad RVL_SDK glob.

set(SMG_DECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../decomp")
set(SMG_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../include")

file(GLOB_RECURSE SMG_GAME_CXX_SOURCES CONFIGURE_DEPENDS
  "${SMG_DECOMP_ROOT}/src/Game/*.cpp")
file(GLOB_RECURSE SMG_GAME_C_SOURCES CONFIGURE_DEPENDS
  "${SMG_DECOMP_ROOT}/src/Game/*.c")
file(GLOB_RECURSE SMG_JSYSTEM_SOURCES CONFIGURE_DEPENDS
  "${SMG_DECOMP_ROOT}/src/JSystem/*.cpp")
file(GLOB_RECURSE SMG_NW4R_SOURCES CONFIGURE_DEPENDS
  "${SMG_DECOMP_ROOT}/src/nw4r/*.cpp")

# These are the complete approved software-only Petari SDK contribution for
# Stage 1. In particular, the duplicate Sunshine GD set is not included.
set(SMG_PETARI_SDK_SOURCES
  "${SMG_DECOMP_ROOT}/src/RVL_SDK/arc/arc.c"
  "${SMG_DECOMP_ROOT}/src/RVL_SDK/aralt/aralt.c"
  "${SMG_DECOMP_ROOT}/src/RVL_SDK/tpl/TPL.c"
  "${SMG_DECOMP_ROOT}/src/RVL_SDK/gd/GDIndirect.c"
  "${SMG_DECOMP_ROOT}/src/RVL_SDK/gd/GDTexture.c")

set(SMG_GAME_SOURCES
  ${SMG_GAME_CXX_SOURCES}
  ${SMG_GAME_C_SOURCES}
  ${SMG_JSYSTEM_SOURCES}
  ${SMG_NW4R_SOURCES}
  ${SMG_PETARI_SDK_SOURCES})

set(SMG_GAME_INCLUDE_DIRS
  "${SMG_INCLUDE_ROOT}"
  "${SMG_DECOMP_ROOT}/include"
  "${SMG_DECOMP_ROOT}/libs/JSystem/include"
  "${SMG_DECOMP_ROOT}/libs/RVL_SDK/include"
  "${SMG_DECOMP_ROOT}/libs/nw4r/include"
  "${SMG_DECOMP_ROOT}/libs/RVLFaceLib/include"
  # Sunshine's shared Wii U providers are built against its pinned Dolphin
  # headers; keep those roots explicit instead of relying on include order.
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/libs/dolphin/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/libs/JSystem/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/libs/PowerPC_EABI_Support/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/libs/TRK_MINNOW_DOLPHIN/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../decomp/libs/OdemuExi2/include")

# Review-only exclusions. Keep these names beside the source list so adding
# a new RVL_SDK glob cannot silently bring a second ABI provider into SMG.
set(SMG_EXCLUDED_PETARI_SDK_FAMILIES
  gx os mem ai dsp ax axfx vi dvd pad kpad wpad)
set(SMG_EXCLUDED_PETARI_GD_SOURCES
  GDBase.c GDGeometry.c GDLight.c GDPixel.c GDTev.c GDTransform.c)
