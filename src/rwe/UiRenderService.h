#pragma once

#include <rwe/ShaderService.h>
#include <algorithm>
#include <rwe/Viewport.h>
#include <rwe/render/GraphicsContext.h>
#include <stack>

namespace rwe
{
    class UiRenderService
    {
    private:
        GraphicsContext* graphics;
        ShaderService* shaders;
        const AbstractViewport* viewport;

        std::stack<Matrix4f> matrixStack{{Matrix4f::identity()}};

        /**
         * Clip rectangles in logical viewport coordinates.
         * An empty optional means no clip is applied.
         */
        std::stack<std::optional<Rectangle2f>> clipStack;

    public:
        UiRenderService(GraphicsContext* graphics, ShaderService* shaders, const AbstractViewport* viewport);

        void fillScreen(const Color& color);

        void drawSprite(float x, float y, const Sprite& sprite);

        void drawSprite(float x, float y, const Sprite& sprite, const Color& tint);

        /** Draws the sprite, ignoring its internal x and y offset. */
        void drawSpriteAbs(float x, float y, const Sprite& sprite);

        void drawSpriteAbs(float x, float y, float width, float height, const Sprite& sprite);

        void drawSpriteAbs(const Rectangle2f& rect, const Sprite& sprite);

        void drawText(float x, float y, const std::string& text, const SpriteSeries& font);
        void drawText(float x, float y, const std::string& text, const SpriteSeries& font, const Color& tint);

        void drawTextWrapped(Rectangle2f area, const std::string& text, const SpriteSeries& font);

        void drawTextCentered(float x, float y, const std::string& text, const SpriteSeries& font);

        void drawTextCenteredX(float x, float y, const std::string& text, const SpriteSeries& font);

        void drawTextAlignRight(float x, float y, const std::string& text, const SpriteSeries& font);
        void drawTextAlignRight(float x, float y, const std::string& text, const SpriteSeries& font, const Color& tint);

        float getTextWidth(const std::string& text, const SpriteSeries& font);

        void pushMatrix();

        void popMatrix();

        /**
         * Pushes a clip rectangle given in the current local
         * coordinate space. It is transformed by the current matrix
         * and intersected with any enclosing clip. All subsequent
         * drawing is clipped to the rectangle until popClip().
         */
        void pushClip(const Rectangle2f& rect);

        void popClip();

        /**
         * Multiplies the current OpenGL matrix with the specified matrix.
         * If the current OpenGL matrix is C and the coordinates to be transformed are v,
         * meaning that the current transformation would be C * v,
         * then calling this with an argument M replaces the current transformation with (C * M) * v.
         * Intuitively this means that any transformation you pass in here
         * will be done directly on the coordinates,
         * and the existing tranformation will be done on the result of that.
         */
        void multiplyMatrix(const Matrix4f& matrix);

        void fillColor(float x, float y, float width, float height, Color color);

        /**
         * Draws a batch of pre-built colored triangles with a single draw
         * call. Equivalent to many fillColor calls sharing the same alpha,
         * but uploads one mesh instead of one mesh per quad.
         */
        void fillColorBatch(const std::vector<GlColoredVertex>& vertices, unsigned char alpha);

        void drawHealthBar(float x, float y, float percentFull);

        void drawHealthBar2(float x, float y, float width, float height, float percentFull);

        void drawBoxOutline(float x, float y, float width, float height, Color color);

        void drawBoxOutline(float x, float y, float width, float height, Color color, float thickness);

        void drawLine(const Vector2f& start, const Vector2f& end);

        /**
         * Draws every vertex pair in `vertices` as a line segment in a
         * single streamed mesh (alpha 1.0). Equivalent to calling
         * drawLine for each pair, without per-line mesh creation.
         */
        void drawLinesBatch(const std::vector<GlColoredVertex>& vertices);

        Matrix4f getViewProjectionMatrix() const;

        Matrix4f getInverseViewProjectionMatrix() const;

    private:
        void applyCurrentClip();
    };

}
