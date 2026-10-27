CXX ?= c++
BUILD_DIR ?= /mnt/buildcache/wiiu-build/sms-gx2r/grass
TEST := $(BUILD_DIR)/grass_gpu_math_test
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Werror -O2

.PHONY: check clean

check: $(TEST)
	$(TEST)

$(TEST): grass_gpu_math_test.cpp ../grass_math.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I.. $< -o $@

clean:
	rm -f $(TEST)
