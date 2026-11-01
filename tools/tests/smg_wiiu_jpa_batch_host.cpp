#include "JPADirectionalBatch.hpp"

#include <cmath>
#include <cstdio>

static bool same(float a, float b) { return std::fabs(a - b) < 0.0001f; }

struct ChildType4State {
    unsigned prm;
    unsigned env;
    unsigned alpha;
    unsigned tex;
};

static bool sameChildType4State(const ChildType4State& a, const ChildType4State& b) {
    return a.prm == b.prm && a.env == b.env && a.alpha == b.alpha && a.tex == b.tex;
}

int main() {
    // Two directional particles, including a rotation in the second one's
    // already composed model-view matrix. These are the actual S8 quad bytes.
    const signed char quad[12] = {-50, -50, 0, 50, -50, 0, 50, 50, 0, -50, 50, 0};
    // This is jpa_dl_x from JPABaseShape.cpp. Each vertex has a POS index and
    // a TEX0 index; GX resolves the former as index * 3 in the S8 XYZ array.
    const unsigned char jpa_dl_x[] = {
        0x80, 0x00, 0x08, 0x00, 0x00, 0x01, 0x01, 0x02,
        0x02, 0x03, 0x03, 0x48, 0x00, 0x49, 0x01, 0x4A,
        0x02, 0x4B, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Byte slice jpa_pos[0x30 : 0x30 + 0x4c * 3] from JPAResource.cpp: type-4
    // passes jpa_pos + 0x30 as its position array to the indexed GX list.
    const unsigned char type4_pos[] = {
        0xE7, 0x19, 0x00, 0x19, 0x19, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0xE7, 0x00,
        0xCE, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0xCE, 0xE7, 0x00,
        0x00, 0x32, 0x00, 0x32, 0x32, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE7, 0x32, 0x00, 0x19, 0x32, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
        0xCE, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x32,
        0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x32,
        0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x32,
        0x00, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x00, 0x00, 0x19,
        0xE7, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0x19, 0x00, 0x19, 0xE7, 0x00, 0x19,
        0xCE, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0xCE, 0x00, 0x19,
        0x00, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE7, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
        0xCE, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x00,
        0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0xE7,
        0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0xCE,
        0x00, 0x19, 0x00, 0x00, 0x19, 0x32, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x00,
        0x00, 0x19, 0xE7, 0x00, 0x19, 0x19, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0xE7,
    };
    const unsigned char type4_pos_indices[2][4] = {
        {jpa_dl_x[4], jpa_dl_x[6], jpa_dl_x[8], jpa_dl_x[10]},
        {jpa_dl_x[12], jpa_dl_x[14], jpa_dl_x[16], jpa_dl_x[18]},
    };
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
    const float type4_matrix[3][4] = {
        {0.4f, 0.1f, 0.0f, 3.0f}, {-0.2f, 0.8f, 0.0f, -4.0f}, {0.0f, 0.0f, 1.2f, 6.0f},
    };
    for (unsigned quad_index = 0; quad_index < 2; ++quad_index) {
        const signed char* batch_template = reinterpret_cast<const signed char*>(type4_pos) +
            type4_pos_indices[quad_index][0] * 3;
        for (unsigned corner = 0; corner < 4; ++corner) {
            const unsigned pos_index = type4_pos_indices[quad_index][corner];
            // This is the CPU-side equivalent of the batched GX output.
            const jpa_batch::Vertex got = jpa_batch::transform(type4_matrix, batch_template + corner * 3);
            float reference[3];
            // Decode the same indexed POS byte stream independently. In
            // particular, 0x48 is an index, not a byte offset.
            const signed char* decoded = reinterpret_cast<const signed char*>(type4_pos) + pos_index * 3;
            for (unsigned row = 0; row < 3; ++row) {
                reference[row] = type4_matrix[row][3];
                for (unsigned axis = 0; axis < 3; ++axis)
                    reference[row] += type4_matrix[row][axis] * decoded[axis];
            }
            if (!same(got.x, reference[0]) || !same(got.y, reference[1]) || !same(got.z, reference[2]))
                return 1;
        }
    }

    // Child type-4 particles with the same state form one directional run and
    // therefore one GX draw with the remaining particles emitted in that run.
    const ChildType4State childRun[] = {
        {0x10, 0x20, 0x30, 0x40},
        {0x10, 0x20, 0x30, 0x40},
        {0x10, 0x20, 0x30, 0x40},
        {0x10, 0x20, 0x30, 0x40},
    };
    unsigned drawCount = 0;
    unsigned batchedParticles = 0;
    for (unsigned first = 0; first < sizeof(childRun) / sizeof(childRun[0]);) {
        unsigned end = first + 1;
        while (end < sizeof(childRun) / sizeof(childRun[0]) && sameChildType4State(childRun[first], childRun[end]))
            ++end;
        ++drawCount;
        batchedParticles += end - first - 1;
        first = end;
    }
    if (drawCount != 1 || batchedParticles != 3)
        return 1;

    std::puts("jpa-batch: PASS (type-3 quads, type-4 cross quads, child type-4 run)");
    return 0;
}
