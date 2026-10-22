# Offline mixer test: make && ./audio_test DISC_FILES_DIR [OUT_DIR]
CXX ?= g++
CXXFLAGS ?= -O2 -g -Wall -std=gnu++11
# Galaxy's protocol test links the DSP HLE too, with the SMG guard on.
SMG_FLAGS = -DPORT_GAME_SMG -I../../../src -I../.. -I../../../decomp/libs/dolphin/include -I../../../decomp/include
audio_test: audio_test.cc ../dsp_mixer.cpp ../dsp_mixer.h
	$(CXX) $(CXXFLAGS) -o $@ audio_test.cc ../dsp_mixer.cpp -lm
mixer_regression: mixer_regression.cc ../dsp_mixer.cpp ../dsp_mixer.h
	$(CXX) $(CXXFLAGS) -o $@ mixer_regression.cc ../dsp_mixer.cpp -lm
smg_galaxy_protocol_test: smg_galaxy_protocol_test.cc ../dsp_hle.cpp ../dsp_mixer.cpp ../dsp_mixer.h
	$(CXX) $(CXXFLAGS) $(SMG_FLAGS) -o $@ smg_galaxy_protocol_test.cc ../dsp_hle.cpp ../dsp_mixer.cpp -lm
smg_wiiu_aram_dma_test: smg_wiiu_aram_dma_test.cc ../../../platform/wiiu/aram_dma.h
	$(CXX) $(CXXFLAGS) -I../../../games/smg/platform/wiiu -I../../../decomp/libs/dolphin/include -o $@ smg_wiiu_aram_dma_test.cc
smg_wiiu_async_test: smg_wiiu_async_test.cc ../dsp_hle.cpp ../dsp_mixer.cpp ../dsp_mixer.h
	$(CXX) $(CXXFLAGS) $(SMG_FLAGS) -DSMG_AUDIO_ASYNC_TEST -pthread -o $@ smg_wiiu_async_test.cc ../dsp_hle.cpp ../dsp_mixer.cpp -lm
check: mixer_regression
	./mixer_regression
clean:
	rm -f audio_test mixer_regression smg_galaxy_protocol_test smg_wiiu_aram_dma_test smg_wiiu_async_test
.PHONY: clean check
