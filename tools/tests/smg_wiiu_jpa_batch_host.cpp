#include "JPADirectionalBatch.hpp"

#include <cmath>
#include <cstdio>

static bool same(float a, float b) { return std::fabs(a - b) < 0.0001f; }

int main() {
    // Two directional particles, including a rotation in the second one's
    // already composed model-view matrix. These are the actual S8 quad bytes.
    const signed char quad[12] = {-50, -50, 0, 50, -50, 0, 50, 50, 0, -50, 50, 0};
    const float matrices[2][3][4] = {
        {{0.5f, 0.0f, 0.0f, 10.0f}, {0.0f, 0.75f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f, -20.0f}},
        {{0.0f, -0.25f, 0.0f, -7.0f}, {0.25f, 0.0f, 0.0f, 14.0f}, {0.0f, 0.0f, 1.5f, -40.0f}},
    };
    jpa_batch::Vertex emitted[8];
    for (unsigned particle = 0; particle < 2; ++particle)
        for (unsigned corner = 0; corner < 4; ++corner)
            emitted[particle * 4 + corner] = jpa_batch::transform(matrices[particle], quad + corner * 3);
    for (unsigned particle = 0; particle < 2; ++particle) {
        for (unsigned corner = 0; corner < 4; ++corner) {
            const signed char* p = quad + corner * 3;
            float reference[3];
            // Independent per-particle GX matrix path: signed-byte vertex is
            // multiplied by the model-view matrix in each component.
            for (unsigned row = 0; row < 3; ++row) {
                reference[row] = matrices[particle][row][3];
                for (unsigned axis = 0; axis < 3; ++axis)
                    reference[row] += matrices[particle][row][axis] * p[axis];
            }
            const jpa_batch::Vertex& got = emitted[particle * 4 + corner];
            if (!same(got.x, reference[0]) || !same(got.y, reference[1]) || !same(got.z, reference[2]))
                return 1;
        }
    }
    std::puts("jpa-batch: PASS (two particles, eight positions)");
    return 0;
}
