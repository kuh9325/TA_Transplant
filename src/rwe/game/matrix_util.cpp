#include "matrix_util.h"
#include <cmath>

namespace rwe
{
    Matrix4f toFloatMatrix(Matrix4x<SimScalar> m)
    {
        Matrix4f n;

        for (int i = 0; i < 16; ++i)
        {
            n.data[i] = simScalarToFloat(m.data[i]);
        }

        return n;
    }

    FrustumPlane makeFrustumPlane(float a, float b, float c, float d)
    {
        FrustumPlane p{Vector3f(a, b, c), d};
        float len = std::sqrt(a * a + b * b + c * c);
        if (len > 0.0f)
        {
            p.normal = Vector3f(a / len, b / len, c / len);
            p.distance = d / len;
        }
        return p;
    }

    ViewFrustum extractFrustum(const Matrix4f& m)
    {
        ViewFrustum f;
        const float* d = m.data;
        // For a column-major matrix where clip = M * p,
        // M[r][c] = data[c*4 + r].
        f.planes[0] = makeFrustumPlane(d[3] + d[0], d[7] + d[4], d[11] + d[8], d[15] + d[12]); // left
        f.planes[1] = makeFrustumPlane(d[3] - d[0], d[7] - d[4], d[11] - d[8], d[15] - d[12]); // right
        f.planes[2] = makeFrustumPlane(d[3] + d[1], d[7] + d[5], d[11] + d[9], d[15] + d[13]); // bottom
        f.planes[3] = makeFrustumPlane(d[3] - d[1], d[7] - d[5], d[11] - d[9], d[15] - d[13]); // top
        f.planes[4] = makeFrustumPlane(d[3] + d[2], d[7] + d[6], d[11] + d[10], d[15] + d[14]); // near
        f.planes[5] = makeFrustumPlane(d[3] - d[2], d[7] - d[6], d[11] - d[10], d[15] - d[14]); // far
        return f;
    }

    bool sphereInFrustum(const ViewFrustum& frustum, const Vector3f& center, float radius)
    {
        for (const auto& p : frustum.planes)
        {
            if (p.normal.dot(center) + p.distance < -radius)
            {
                return false;
            }
        }
        return true;
    }
}
