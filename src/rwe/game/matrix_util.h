#pragma once

#include <rwe/math/Matrix4f.h>
#include <rwe/math/Vector3f.h>
#include <rwe/sim/SimScalar.h>

namespace rwe
{
    Matrix4f toFloatMatrix(Matrix4x<SimScalar> m);

    /**
     * A clip-space plane extracted from a view-projection matrix.
     * Points with dot(normal, p) + distance < 0 are outside.
     */
    struct FrustumPlane
    {
        Vector3f normal;
        float distance;
    };

    struct ViewFrustum
    {
        FrustumPlane planes[6];
    };

    /**
     * Extracts the six view-frustum planes from a combined
     * view-projection matrix (Gribb-Hartmann method).
     */
    ViewFrustum extractFrustum(const Matrix4f& viewProjection);

    /**
     * Conservative sphere-frustum test. Returns true if the sphere
     * intersects or lies inside the frustum.
     */
    bool sphereInFrustum(const ViewFrustum& frustum, const Vector3f& center, float radius);

}
