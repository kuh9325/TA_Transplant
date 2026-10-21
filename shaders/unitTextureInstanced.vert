#version 150

// instanced variant of unitTexture: per-piece transforms and flags arrive
// as instance attributes instead of uniforms. iModel occupies attribute
// locations 3-6, iParams location 7, iMvp locations 8-11. iMvp is the
// CPU-precomputed vp*model so projected positions round identically to
// the per-draw path.
in vec3 position;
in vec2 texCoord;
in vec3 normal;
in mat4 iModel;
in vec4 iParams; // x = shade flag, y = alpha
in mat4 iMvp;

out vec2 fragTexCoord;
out float height;
out vec3 worldNormal;
flat out float instShade;
flat out float instAlpha;

void main(void)
{
    vec4 worldPosition = iModel * vec4(position, 1.0);
    gl_Position = iMvp * vec4(position, 1.0);
    fragTexCoord = texCoord;
    height = worldPosition.y;
    worldNormal = mat3(iModel) * normal;
    instShade = iParams.x;
    instAlpha = iParams.y;
}
