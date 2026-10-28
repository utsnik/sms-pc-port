#include "../widescreen_math.h"

#include <cstdio>

namespace {

int failures;

void check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    const gx::TargetScissor wide = gx::fullTargetScissor(1920, 1080);
    check(wide.left == 0 && wide.top == 0 && wide.width == 1920 && wide.height == 1080,
          "widescreen frame clear restores the full target");

    const gx::TargetScissor standard = gx::fullTargetScissor(640, 480);
    check(standard.left == 0 && standard.top == 0 && standard.width == 640 && standard.height == 480,
          "4:3 frame clear keeps the full target");
    return failures ? 1 : 0;
}
