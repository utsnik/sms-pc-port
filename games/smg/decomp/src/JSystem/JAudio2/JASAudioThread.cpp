#include "JSystem/JAudio2/JASAudioThread.hpp"
#include "JSystem/JAudio2/JASAiCtrl.hpp"
#include "JSystem/JAudio2/JASChannel.hpp"
#include "JSystem/JAudio2/JASDSPChannel.hpp"
#include "JSystem/JAudio2/JASDSPInterface.hpp"
#include "JSystem/JAudio2/JASDriverIF.hpp"
#include "JSystem/JAudio2/JASHeapCtrl.hpp"
#include "JSystem/JAudio2/JASProbe.hpp"
#include "JSystem/JAudio2/JASReport.hpp"
#include "JSystem/JKernel/JKRSolidHeap.hpp"
#include <revolution/dsp.h>
#include <stdint.h>

inline JASAudioThread::JASAudioThread(int stackSize, int msgCount, u32 threadPriority)
    : JKRThread(JASDram, threadPriority, msgCount, stackSize), JASGlobalInstance< JASAudioThread >(true) {
    sbPauseFlag = false;
    OSInitThreadQueue(&sThreadQueue);
}

#ifdef TARGET_WIIU
/* On the Wii the priority-2 audio thread runs its whole init (DSP boot, which
 * busy-waits on the DSP, then FX_BUF and the channel pools from JASDram) the
 * moment create() resumes it, before the creating thread runs again. The Wii U
 * DSP HLE answers from other threads, so the audio thread can lose the core
 * mid-boot, and the creator went on to JAUNewSectionHeap(), which takes all of
 * JASDram's free space: FX_BUF's 0x6000-byte JASDram allocation then failed
 * (HeapMemoryWatcher OSPanic). Keep the retail order: wait for the init. */
static volatile bool sInitDone;
#endif

void JASAudioThread::create(s32 threadPriority) {
    const int size = 0x2000;
    JASAudioThread* pAudioThread = new (JASDram, 0) JASAudioThread(threadPriority, 0x10, size);
    JKRHeap* pCurrentHeap = JKRGetSystemHeap();
    pAudioThread->setCurrentHeap(pCurrentHeap);
    pAudioThread->resume();
#ifdef TARGET_WIIU
    while (!sInitDone) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
#endif
}

void JASAudioThread::stop() {
    jamMessageBlock((void*)2);
}

volatile int JASAudioThread::snIntCount;

void* JASAudioThread::run() {
    OSInitFastCast();
    JASDriver::initAI(DMACallback);
    JASDsp::boot(DSPCallback);
    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded< JASChannel >::newMemPool(0x48);
    JASDriver::startDMA();
#ifdef TARGET_WIIU
    sInitDone = true;
#endif

    OSMessage msg;
    while (true) {
        msg = waitMessageBlock();
        switch ((intptr_t)msg) {
        case AUDIOMSG_DMA:
            if (sbPauseFlag) {
                JASDriver::stopDMA();
                OSSleepThread(&sThreadQueue);
            }

            JASDriver::updateDac();
            JASDriver::updateDacCallback();
            continue;

        case AUDIOMSG_DSP:
            snIntCount -= 1;
            if (snIntCount == 0) {
                JASProbe::stop(7);
                JASDriver::finishDSPFrame();
            } else {
                JASProbe::start(2, "SFR_DSP");
                JASDriver::updateDSP();
                JASProbe::stop(2);
            }

            continue;

        case AUDIOMSG_STOP:
            JASDriver::stopDMA();
            OSExitThread(nullptr);
            continue;
        }
    }
}

void JASAudioThread::DMACallback() {
    JASAudioThread* pAudioThread = getInstance();
    JASProbe::stop(4);
    JASProbe::start(4, "UPDATE-DAC");
    if (!pAudioThread->sendMessage((void*)AUDIOMSG_DMA)) {
    }
}

void JASAudioThread::DSPCallback(void*) {
    JASAudioThread* pAudioThread = getInstance();
    while (DSPCheckMailFromDSP() == 0) {
    }

    u32 mail = DSPReadMailFromDSP();
    if (mail >> 0x10 != 0xF355) {
    } else {
        if ((mail & 0xFF00) == 0xFF00) {
            if (!pAudioThread->sendMessage((void*)AUDIOMSG_DSP)) {
            }
        } else {
            JASDsp::finishWork(mail);
        }
    }
}
