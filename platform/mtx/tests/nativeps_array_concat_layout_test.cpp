#include "../nativeps_check_layout.h"

#include <cassert>
#include <cstdint>
#include <cstring>

int main()
{
	float a[nativeps_check::kArrayConcatARows][nativeps_check::kArrayConcatWordsPerRow];
	float b[nativeps_check::kArrayConcatBRows][nativeps_check::kArrayConcatWordsPerRow];
	float native[nativeps_check::kArrayConcatBRows][nativeps_check::kArrayConcatWordsPerRow];
	float c[nativeps_check::kArrayConcatBRows][nativeps_check::kArrayConcatWordsPerRow];
	assert(sizeof(a) / sizeof(float) == nativeps_check::kArrayConcatAWords);
	assert(sizeof(b) / sizeof(float) == nativeps_check::kArrayConcatBWords);
	assert(sizeof(native) / sizeof(float) == nativeps_check::kArrayConcatBWords);
	assert(sizeof(c) / sizeof(float) == nativeps_check::kArrayConcatBWords);

	struct Input {
		uint32_t words[nativeps_check::kArrayConcatInputWords];
		uint32_t guard;
	} input;
	input.guard = 0xa55a5aa5u;
	std::memcpy(input.words, a, sizeof(a));
	std::memcpy(input.words + nativeps_check::kArrayConcatAWords, b, sizeof(b));
	input.words[nativeps_check::kArrayConcatInputWords - 1] = 4;
	assert(input.guard == 0xa55a5aa5u);

	for (unsigned matrix = 0; matrix < 4; ++matrix) {
		const unsigned row = matrix * nativeps_check::kArrayConcatRowsPerMatrix;
		assert(&native[row][0] - &native[0][0] ==
		       matrix * nativeps_check::kArrayConcatOutputWords);
		assert(&c[row][0] - &c[0][0] ==
		       matrix * nativeps_check::kArrayConcatOutputWords);
	}
	return 0;
}
