#version 150

// instanced variant of unitShadow: per-piece model transform and caster
// ground height arrive as instance attributes. iModel = locations 3-6,
// iParams = location 7, iParams.x = groundHeight. The shadow vertex is a
// terrain-projected point, so it keeps the shared vpMatrix uniform
// rather than the per-instance iMvp.
uniform mat4 vpMatrix;

in vec3 position;
in vec2 texCoord;
in mat4 iModel;
in vec4 iParams;

out vec2 fragTexCoord;
out float height;
flat out float instGroundHeight;

void main(void)
{
    float groundHeight = iParams.x;
    vec4 worldPosition = iModel * vec4(position, 1.0);

    float shadowOffset = (worldPosition.y - groundHeight) * 0.25;

    vec4 shadowPosition = vec4(
        worldPosition.x + shadowOffset,
        groundHeight,
        worldPosition.z - shadowOffset,
        1.0);

    gl_Position = vpMatrix * shadowPosition;
    fragTexCoord = texCoord;
    height = worldPosition.y;
    instGroundHeight = groundHeight;
}
