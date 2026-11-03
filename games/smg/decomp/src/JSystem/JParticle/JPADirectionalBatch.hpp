#pragma once

// The GX template is signed-byte XYZ. Keep this transform independent of GX so
// the host test exercises the same vertex materialization as the Wii U path.
class JPAEmitterWorkData;
class JPABaseParticle;
class JPAParticleCallBack;

namespace jpa_batch {
struct Vertex {
    float x, y, z;
};

inline Vertex transform(const float matrix[3][4], const signed char* xyz) {
    Vertex v;
    v.x = matrix[0][0] * xyz[0] + matrix[0][1] * xyz[1] + matrix[0][2] * xyz[2] + matrix[0][3];
    v.y = matrix[1][0] * xyz[0] + matrix[1][1] * xyz[1] + matrix[1][2] * xyz[2] + matrix[1][3];
    v.z = matrix[2][0] * xyz[0] + matrix[2][1] * xyz[1] + matrix[2][2] * xyz[2] + matrix[2][3];
    return v;
}

// Returns false for the same invisible/degenerate cases as the legacy visitor.
bool eligible(::JPAEmitterWorkData*, ::JPABaseParticle*);
bool materialize(::JPAEmitterWorkData*, ::JPABaseParticle*, const signed char* template_xyz,
                 Vertex out[4], float matrix[3][4], bool rotated);
// SMG's concrete particle callbacks only update particles in execute(); their
// inherited draw() is the empty JPA default. Unknown callback hierarchies must
// remain on the legacy per-particle path.
bool callbackDrawsNothing(::JPAParticleCallBack*);
}
