#include "Game/System/HomeButtonMenuWrapper.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <revolution/rso.h>

#if defined(TARGET_WIIU)
// OSReport is a weak SDK stub on this target (platform/sdk_stubs.cpp) that
// only counts the call and discards the message, so it cannot carry this
// log line. Use the same WHBLogPrintf path the Stage 1/2 stubs already use
// (games/smg/platform/wiiu/sdk_stubs.cpp) to make it observable in Cemu.
#include <whb/log.h>
#endif

#ifndef TARGET_PC
void (*HBMCreateRSO)(const HBMDataInfo*);
void (*HBMInitRSO)(void);
void (*HBMCalcRSO)(const HBMControllerData*);
void (*HBMDrawRSO)(void);
HBMSelectBtnNum (*HBMGetSelectBtnNumRSO)(void);
void (*HBMSetAdjustFlagRSO)(int);
void (*HBMStartBlackOutRSO)(void);

static RSOExportFuncTable exp_tbl[] = {{"HBMCreateRSO", (u32*)&HBMCreateRSO},
                                       {"HBMInitRSO", (u32*)&HBMInitRSO},
                                       {"HBMCalcRSO", (u32*)&HBMCalcRSO},
                                       {"HBMDrawRSO", (u32*)&HBMDrawRSO},
                                       {"HBMGetSelectBtnNumRSO", (u32*)&HBMGetSelectBtnNumRSO},
                                       {"HBMSetAdjustFlagRSO", (u32*)&HBMSetAdjustFlagRSO},
                                       {"HBMStartBlackOutRSO", (u32*)&HBMStartBlackOutRSO}};

typedef void (*ProloguePtr)(BOOL);

void RSO::setupRsoHomeButtonMenu() {
    u32 i;
    RSOObjectHeader* rsoPtr;
    RSOExportFuncTable* pTbl;
    const RSOObjectHeader* symbolTable = reinterpret_cast< const RSOObjectHeader* >(MR::receiveFile("/ModuleData/product.sel"));
    int jumpCodeSize;
    void* bss;
    void* jumps;

    RSOListInit((void*)symbolTable);
    if (symbolTable != nullptr) {
        jumpCodeSize = RSOGetJumpCodeSize(symbolTable);

        if (jumpCodeSize == 0) {
            jumps = nullptr;
        } else {
            jumps = new (MR::getStationedHeapGDDR3(), 0) char[jumpCodeSize];
            RSOMakeJumpCode(symbolTable, jumps);
        }
        rsoPtr = reinterpret_cast< RSOObjectHeader* >(MR::receiveFile("/ModuleData/HomeButtonMenuWrapperRSO.rso"));
        if (rsoPtr->mBssSize) {
            bss = new (MR::getStationedHeapGDDR3(), 0) char[rsoPtr->mBssSize];
        }

        RSOLinkList((void*)rsoPtr, bss);

        if (rsoPtr != nullptr) {
            RSOLinkJump(rsoPtr, symbolTable, jumps);
            reinterpret_cast< ProloguePtr >(rsoPtr->mProlog)(RSOIsImportSymbolResolvedAll(rsoPtr));
            for (i = 0; i < ARRAY_SIZE(exp_tbl); i++) {
                pTbl = &exp_tbl[i];
                RSOFindExportSymbolAddr(rsoPtr, pTbl->symbol_name);
                *(pTbl->symbol_ptr) = (u32)RSOFindExportSymbolAddr(rsoPtr, pTbl->symbol_name);
            }
        }
    }
}

void RSO::HBMCreate(const HBMDataInfo* pHBInfo) {
    (*HBMCreateRSO)(pHBInfo);
}

void RSO::HBMInit() {
    (*HBMInitRSO)();
}

void RSO::HBMCalc(const HBMControllerData* pController) {
    (*HBMCalcRSO)(pController);
}

void RSO::HBMDraw() {
    (*HBMDrawRSO)();
}

HBMSelectBtnNum RSO::HBMGetSelectBtnNum() {
    return (*HBMGetSelectBtnNumRSO)();
}

void RSO::HBMSetAdjustFlag(int flag) {
    (*HBMSetAdjustFlagRSO)(flag);
}

void RSO::HBMStartBlackOut() {
    (*HBMStartBlackOutRSO)();
}

#else
// The HOME Menu ships as a PowerPC module (HomeButtonMenuWrapperRSO.rso).
// On Quest the system menu takes its place, so the menu closes as soon as
// it opens. On Wii U, Cemu has no HOME RSO to link either; Stage 2 leaves
// this a deterministic no-op and HBMGetSelectBtnNum() below already returns
// the neutral HBM_SELECT_HOMEBTN value.
void RSO::setupRsoHomeButtonMenu() {
#if defined(TARGET_WIIU)
    WHBLogPrintf("SMG Wii U: HOME RSO disabled for Stage 2 Cemu");
#endif
}

void RSO::HBMCreate(const HBMDataInfo* pHBInfo) {
}

void RSO::HBMInit() {
}

void RSO::HBMCalc(const HBMControllerData* pController) {
}

void RSO::HBMDraw() {
}

HBMSelectBtnNum RSO::HBMGetSelectBtnNum() {
    return HBM_SELECT_HOMEBTN;
}

void RSO::HBMSetAdjustFlag(int flag) {
}

void RSO::HBMStartBlackOut() {
}
#endif

void _unresolved(void) {
}
