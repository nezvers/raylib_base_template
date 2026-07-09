#include "box2d_wrap.h"
#include "raylib.h"
#include "raymath.h"

static void DrawPolygonFcn( const b2Vec2* vertices, int vertexCount, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    for (int i = 0; i < vertexCount - 1; i += 1) {
        b2Vec2 v1 = vertices[i];
        b2Vec2 v2 = vertices[i+1];
        DrawLineV((Vector2){v1.x, -v1.y}, (Vector2){v2.x, -v2.y}, rl_color);
    }
    // connect end points
    b2Vec2 v1 = vertices[vertexCount - 1];
    b2Vec2 v2 = vertices[0];
    DrawLineV((Vector2){v1.x, -v1.y}, (Vector2){v2.x, -v2.y}, rl_color);
}

static void DrawSolidPolygonFcn( b2Transform transform, const b2Vec2* vertices, int vertexCount, float radius, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    
    Vector2 buffer[255]; // TODO: Be sure buffer size is enough
    for (int i = 0; i < vertexCount; i += 1) {
        b2Vec2 v = b2RotateVector(transform.q, vertices[i]);
        buffer[i] = Vector2Add(*(Vector2*)& v, *(Vector2*)& transform.p);
    }
    // TODO: potentially have more than 2x triangles
    DrawTriangle(buffer[0], buffer[1], buffer[2], rl_color);
    DrawTriangle(buffer[0], buffer[2], buffer[3], rl_color);
}

static void DrawCircleFcn( b2Vec2 center, float radius, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    DrawCircleLinesV((Vector2){center.x, -center.y}, radius, rl_color);
}

static void DrawSolidCircleFcn( b2Transform transform, float radius, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    // TODO: Draw line for rotation
    DrawCircleV((Vector2){transform.p.x, -transform.p.y}, radius, rl_color);
}

static void DrawSolidCapsuleFcn( b2Vec2 p1, b2Vec2 p2, float radius, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);

    DrawCircleV((Vector2){p1.x, -p1.y}, radius, rl_color);
    DrawCircleV((Vector2){p2.x, -p2.y}, radius, rl_color);
    // TODO: Draw with polygon, potentially rotates
    // TODO: check that p1 is on top
    DrawRectangleRec((Rectangle){p1.x-radius, -p1.y, radius * 2, p1.y - p2.y}, rl_color);
}

static void DrawSegmentFcn( b2Vec2 p1, b2Vec2 p2, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    DrawLineV((Vector2){p1.x, -p1.y}, (Vector2){p2.x, -p2.y}, rl_color);
}

static void DrawStringFcn( b2Vec2 p, const char* s, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    DrawText(s, p.x, -p.y, 10, rl_color);
}

static void DrawTransformFcn( b2Transform transform, void* context ) {
    b2Vec2 p2 = b2Add(transform.p, b2RotateVector(transform.q, (b2Vec2){10, 0}));
    DrawSegmentFcn(transform.p, p2, b2_colorBox2DYellow, context);
}

static void DrawPointFcn( b2Vec2 p, float size, b2HexColor color, void* context ) {
    unsigned int b2_color = (color << 8 | 255);
    Color rl_color = GetColor(b2_color);
    DrawCircle(p.x, -p.y, size, rl_color);
}

b2DebugDraw Box2dRaylibDebugDraw() {
    b2DebugDraw result = b2DefaultDebugDraw();
    result.DrawPolygonFcn = DrawPolygonFcn;
    result.DrawSolidPolygonFcn = DrawSolidPolygonFcn;
    result.DrawCircleFcn = DrawCircleFcn;
    result.DrawSolidCircleFcn = DrawSolidCircleFcn;
    result.DrawSolidCapsuleFcn = DrawSolidCapsuleFcn;
    result.DrawSegmentFcn = DrawSegmentFcn;
    result.DrawStringFcn = DrawStringFcn;
    result.DrawTransformFcn = DrawTransformFcn;
    result.DrawPointFcn = DrawPointFcn;
    return result;
}
