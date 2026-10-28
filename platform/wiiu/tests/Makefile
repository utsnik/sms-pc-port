CXX ?= c++
BUILD_DIR ?= /mnt/buildcache/wiiu-build/sms-gx2r/grass
TEST := $(BUILD_DIR)/grass_gpu_math_test
ORACLE := $(BUILD_DIR)/latte_vs_oracle_test
# No FMA contraction: the oracle compares against unfused Latte arithmetic.
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Werror -O2 -ffp-contract=off

.PHONY: check check-sanitize clean

check: $(TEST) $(ORACLE)
	$(TEST)
	$(ORACLE)

check-sanitize:
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" \
		TEST=$(BUILD_DIR)/grass_gpu_math_test-san ORACLE=$(BUILD_DIR)/latte_vs_oracle_test-san check

$(TEST): grass_gpu_math_test.cpp ../grass_math.h ../gx2_vertex_program.h ../gx2_shader_inl.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-parentheses -I.. $< -o $@

$(ORACLE): latte_vs_oracle_test.cpp latte_vs_interp.h ../gx2_vertex_program.h ../gx2_shader_inl.h ../grass_math.h
	mkdir -p $(BUILD_DIR)
	# gx2_shader_inl.h's macros (shared with lus-rebase) leave sums unparenthesised.
	$(CXX) $(CXXFLAGS) -Wno-parentheses -I.. $< -o $@

clean:
	rm -f $(TEST) $(ORACLE) $(BUILD_DIR)/grass_gpu_math_test-san $(BUILD_DIR)/latte_vs_oracle_test-san
