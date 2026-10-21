#version 150

// instanced variant of unitBuild: per-piece model transform plus the
// per-unit build parameters arrive as instance attributes.
// iParams = (shade, percentComplete, unitY, unused); iMvp (locations
// 8-11) is the CPU-precomputed vp*model so projected positions round
// identically to the per-draw path.

in vec3 position;
in vec2 texCoord;
in vec3 normal;
in mat4 iModel;
in vec4 iParams;
in mat4 iMvp;

out vec2 fragTexCoord;
out float height;
out vec3 worldNormal;
flat out float instShade;
flat out float instPercentComplete;
flat out float instUnitY;

void main(void)
{
    vec4 worldPosition = iModel * vec4(position, 1.0);
    gl_Position = iMvp * vec4(position, 1.0);
    fragTexCoord = texCoord;
    height = worldPosition.y;
    worldNormal = mat3(iModel) * normal;
    instShade = iParams.x;
    instPercentComplete = iParams.y;
    instUnitY = iParams.z;
}
