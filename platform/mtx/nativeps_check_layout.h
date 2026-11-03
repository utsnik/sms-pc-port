#ifndef SMS_NATIVEPS_CHECK_LAYOUT_H
#define SMS_NATIVEPS_CHECK_LAYOUT_H

namespace nativeps_check {

enum {
	kArrayConcatARows = 3,
	kArrayConcatBRows = 12,
	kArrayConcatRowsPerMatrix = 3,
	kArrayConcatWordsPerRow = 4,
	kArrayConcatAWords = kArrayConcatARows * kArrayConcatWordsPerRow,
	kArrayConcatBWords = kArrayConcatBRows * kArrayConcatWordsPerRow,
	kArrayConcatOutputWords = kArrayConcatRowsPerMatrix * kArrayConcatWordsPerRow,
	kArrayConcatInputWords = kArrayConcatAWords + kArrayConcatBWords + 1,
};

} // namespace nativeps_check

#endif
