/*
  Waveshare ST7789 172x320 3D Cube Demo on ESP32-S3

  Description:
    - 3D cube rendering using LovyanGFX sprites and software projection.
    - Uses model rotation, 2D face culling, depth-sorted polygon draw order, and lighting.
    - Inline constant values for hardware pins and animation params (no local #defines used).

  Flow:
    1. Custom LGFX class setup for SPI, panel, backlight.
    2. Cube geometry in normalized coordinates.
    3. Rotate, project, shade, and draw faces every loop.
    4. FPS counter updated on interval.

  Key concepts:
    - Object-space vertices -> rotated world-space -> project to screen.
    - Back-face culling based on 2D screen winding.
    - Phong-style shading: ambient + diffuse + specular.

  Contact:
    - Sayed Ahmadreza Razian
    - AhmadrezaRazian@gmail.com
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include <math.h>

using LGFX_Device    = lgfx::LGFX_Device;
using Panel_ST7789   = lgfx::Panel_ST7789;
using Bus_SPI        = lgfx::Bus_SPI;
using Light_PWM      = lgfx::Light_PWM;

// ======================================================
// LovyanGFX custom display class
// ======================================================
// LGFX wrapper using LovyanGFX on ST7789 172x320 panel.
// This sets up SPI, panel and backlight for the cube renderer.
class LGFX : public LGFX_Device {
  Panel_ST7789 _panel_instance;
  Bus_SPI      _bus_instance;
  Light_PWM    _light_instance;

public:
  LGFX() {
    // SPI bus config (host, speed, and pins)
    { auto cfg = _bus_instance.config(); cfg.spi_host = SPI3_HOST; cfg.spi_mode = 0; cfg.freq_write = 80000000; cfg.freq_read = 16000000; cfg.spi_3wire = false; cfg.use_lock = true; cfg.dma_channel = SPI_DMA_CH_AUTO; cfg.pin_sclk = 40; cfg.pin_mosi = 45; cfg.pin_miso = -1; cfg.pin_dc = 41; _bus_instance.config(cfg); _panel_instance.setBus(&_bus_instance); }

    // Panel config (size, offset, orientation)
    { auto cfg = _panel_instance.config(); cfg.pin_cs = 42; cfg.pin_rst = 39; cfg.pin_busy = -1; cfg.memory_width = 320; cfg.memory_height = 172; cfg.panel_width = 172; cfg.panel_height = 320; cfg.offset_x = 34; cfg.offset_y = 0; cfg.offset_rotation = 0; cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1; cfg.readable = false; cfg.invert = true; cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = false; _panel_instance.config(cfg); }

    // Backlight control
    { auto cfg = _light_instance.config(); cfg.pin_bl = 48; cfg.invert = false; cfg.freq = 44100; cfg.pwm_channel = 7; _light_instance.config(cfg); _panel_instance.setLight(&_light_instance); }

    setPanel(&_panel_instance);
  }
};

// ======================================================
// DATA TYPES
// ======================================================
struct Vec3
{
  float x;
  float y;
  float z;
};

struct Point2
{
  int16_t x;
  int16_t y;
};

struct FaceInfo
{
  uint8_t idx[4];
  uint16_t baseColor;
  float zavg;
  float shade;
  bool visible;
};

// ======================================================
// GLOBALS
// ======================================================
LGFX lcd;
LGFX_Sprite sprite(&lcd);

float g_angX = 0.0f;
float g_angY = 0.0f;
float g_angZ = 0.0f;

uint32_t g_fpsFrameCount = 0;
uint32_t g_fpsLastTickMs = 0;
float g_fps = 0.0f;

bool g_spriteReady = false;

// ======================================================
// CUBE GEOMETRY
// ======================================================
// Unit cube vertices in object space (centered at origin, size=2).
static const Vec3 g_cubeVertices[8] =
{
  { -1, -1, -1 },  // 0
  {  1, -1, -1 },  // 1
  {  1,  1, -1 },  // 2
  { -1,  1, -1 },  // 3
  { -1, -1,  1 },  // 4
  {  1, -1,  1 },  // 5
  {  1,  1,  1 },  // 6
  { -1,  1,  1 }   // 7
};

// IMPORTANT: this order is corrected
static const FaceInfo g_faceTemplate[6] =
{
  {{4, 5, 6, 7}, TFT_RED,   0, 1, true}, // front
  {{1, 0, 3, 2}, TFT_BLUE,  0, 1, true}, // back
  {{0, 4, 7, 3}, TFT_GREEN, 0, 1, true}, // left
  {{5, 1, 2, 6}, TFT_YELLOW,0, 1, true}, // right
  {{3, 7, 6, 2}, TFT_CYAN,  0, 1, true}, // top
  {{0, 1, 5, 4}, TFT_MAGENTA,0, 1, true}  // bottom
};

// ======================================================
// MATH HELPERS
// ======================================================
// Rotation routines around cube axes. Used per-frame for animation.
static Vec3 RotateX(const Vec3& p, float a)
{
  float c = cosf(a);
  float s = sinf(a);
  return { p.x, p.y * c - p.z * s, p.y * s + p.z * c };
}

static Vec3 RotateY(const Vec3& p, float a)
{
  float c = cosf(a);
  float s = sinf(a);
  return { p.x * c + p.z * s, p.y, -p.x * s + p.z * c };
}

static Vec3 RotateZ(const Vec3& p, float a)
{
  float c = cosf(a);
  float s = sinf(a);
  return { p.x * c - p.y * s, p.x * s + p.y * c, p.z };
}

static float Dot3(const Vec3& a, const Vec3& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 Cross3(const Vec3& a, const Vec3& b)
{
  return
  {
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x
  };
}

static float Length3(const Vec3& v)
{
  return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static Vec3 Normalize3(const Vec3& v)
{
  // Normalize vector; protects division by zero.
  float len = Length3(v);
  if (len < 0.00001f) return {0,0,0};
  return { v.x / len, v.y / len, v.z / len };
}

static void NormalizeAngle(float& a)
{
  const float PI2 = 6.28318530718f;
  while (a > PI2) a -= PI2;
  while (a < 0.0f) a += PI2;
}

static Point2 ProjectPoint(const Vec3& p, int centerX, int centerY, float perspective)
{
  float z = p.z + 150.0f;
  if (z < 1.0f) z = 1.0f;

  float k = perspective / z;
  Point2 out;
  out.x = (int16_t)(centerX + p.x * k);
  out.y = (int16_t)(centerY + p.y * k);
  return out;
}

static uint16_t Shade565(uint16_t color, float shade)
{
  if (shade < 0.0f) shade = 0.0f;
  if (shade > 1.0f) shade = 1.0f;

  uint8_t r5 = (color >> 11) & 0x1F;
  uint8_t g6 = (color >> 5)  & 0x3F;
  uint8_t b5 =  color        & 0x1F;

  int r = (int)(r5 * shade);
  int g = (int)(g6 * shade);
  int b = (int)(b5 * shade);

  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void SortFacesByDepth(FaceInfo faces[], int count)
{
  for (int i = 0; i < count - 1; i++)
  {
    for (int j = i + 1; j < count; j++)
    {
      if (faces[i].zavg > faces[j].zavg)
      {
        FaceInfo t = faces[i];
        faces[i] = faces[j];
        faces[j] = t;
      }
    }
  }
}

// ======================================================
// DRAW HELPERS
// ======================================================
static void FillQuad(const Point2& p0, const Point2& p1, const Point2& p2, const Point2& p3, uint16_t color)
{
  sprite.fillTriangle(p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, color);
  sprite.fillTriangle(p0.x, p0.y, p2.x, p2.y, p3.x, p3.y, color);
}

static void DrawQuadBorder(const Point2& p0, const Point2& p1, const Point2& p2, const Point2& p3, uint16_t color)
{
  sprite.drawLine(p0.x, p0.y, p1.x, p1.y, color);
  sprite.drawLine(p1.x, p1.y, p2.x, p2.y, color);
  sprite.drawLine(p2.x, p2.y, p3.x, p3.y, color);
  sprite.drawLine(p3.x, p3.y, p0.x, p0.y, color);
}

static void DrawFPSOnSprite()
{
  char text[32];
  snprintf(text, sizeof(text), "FPS: %.1f", g_fps);

  sprite.fillRect(4, 4, 84, 20, TFT_BLACK);
  sprite.setTextColor(TFT_GREEN, TFT_BLACK);
  sprite.setFont(&fonts::Font2);
  sprite.setCursor(4, 4);
  sprite.print(text);
}

static void AdvanceAngles(float speed)
{
  g_angX += 0.021f * speed;
  g_angY += 0.031f * speed;
  g_angZ += 0.017f * speed;

  NormalizeAngle(g_angX);
  NormalizeAngle(g_angY);
  NormalizeAngle(g_angZ);
}

// ======================================================
// SCREEN-SPACE FACE TEST
// This is the key fix.
// On this display, for the chosen vertex order, front-facing
// projected faces come out with NEGATIVE signed area.
// ======================================================
static bool IsFrontFace2D(const Point2& p0, const Point2& p1, const Point2& p2)
{
  long area2 =
    (long)(p1.x - p0.x) * (long)(p2.y - p0.y) -
    (long)(p1.y - p0.y) * (long)(p2.x - p0.x);

  return area2 < 0;
}

// ======================================================
// MAIN RENDERER
// ======================================================
static float Clamp01(float v)
{
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static Vec3 Reflect3(const Vec3& i, const Vec3& n)
{
  float d = 2.0f * Dot3(i, n);
  return
  {
    i.x - d * n.x,
    i.y - d * n.y,
    i.z - d * n.z
  };
}

// Apply brightness and specular highlight in RGB565.
static uint16_t Shade565_Lit(uint16_t baseColor, float brightness, float specular)
{
  // Clamp lighting factors to [0,1] range
  brightness = Clamp01(brightness);
  specular   = Clamp01(specular);

  int r = (baseColor >> 11) & 0x1F;
  int g = (baseColor >> 5)  & 0x3F;
  int b =  baseColor        & 0x1F;

  float rf = r / 31.0f;
  float gf = g / 63.0f;
  float bf = b / 31.0f;

  // keep color alive, not too dark
  rf = rf * brightness + specular * 0.85f;
  gf = gf * brightness + specular * 0.85f;
  bf = bf * brightness + specular * 0.85f;

  rf = Clamp01(rf);
  gf = Clamp01(gf);
  bf = Clamp01(bf);

  int r2 = (int)(rf * 31.0f + 0.5f);
  int g2 = (int)(gf * 63.0f + 0.5f);
  int b2 = (int)(bf * 31.0f + 0.5f);

  return (uint16_t)((r2 << 11) | (g2 << 5) | b2);
}
// Render a rotating 3D cube into the sprite buffer and push to display.
// centerX/centerY = projection center, size = cube scale.
// perspective = focal length, speed = animation delta multiplier.
void Render_3DBox(int centerX, int centerY, float size, float perspective, float speed)
{
  if (!g_spriteReady) return;

  Vec3 rotated[8];
  Point2 pts[8];
  FaceInfo faces[6];

  sprite.fillScreen(TFT_BLACK);

  Vec3 lightDir = Normalize3({ -0.65f, -0.35f, 1.00f });
  Vec3 cameraPos = { 0.0f, 0.0f, 150.0f };

  // rotate + project
  for (int i = 0; i < 8; i++)
  {
    Vec3 p = g_cubeVertices[i];
    p.x *= size;
    p.y *= size;
    p.z *= size;

    p = RotateX(p, g_angX);
    p = RotateY(p, g_angY);
    p = RotateZ(p, g_angZ);

    rotated[i] = p;
    pts[i] = ProjectPoint(p, centerX, centerY, perspective);
  }

  // build faces
  for (int i = 0; i < 6; i++)
  {
    faces[i] = g_faceTemplate[i];

    const Vec3& v0 = rotated[faces[i].idx[0]];
    const Vec3& v1 = rotated[faces[i].idx[1]];
    const Vec3& v2 = rotated[faces[i].idx[2]];
    const Vec3& v3 = rotated[faces[i].idx[3]];

    faces[i].zavg = (v0.z + v1.z + v2.z + v3.z) * 0.25f;

    Vec3 e1 = { v1.x - v0.x, v1.y - v0.y, v1.z - v0.z };
    Vec3 e2 = { v2.x - v0.x, v2.y - v0.y, v2.z - v0.z };
    Vec3 n = Normalize3(Cross3(e1, e2));

    Vec3 faceCenter =
    {
      (v0.x + v1.x + v2.x + v3.x) * 0.25f,
      (v0.y + v1.y + v2.y + v3.y) * 0.25f,
      (v0.z + v1.z + v2.z + v3.z) * 0.25f
    };

    Vec3 viewDir =
    {
      cameraPos.x - faceCenter.x,
      cameraPos.y - faceCenter.y,
      cameraPos.z - faceCenter.z
    };
    viewDir = Normalize3(viewDir);

        // screen-space culling: only draw faces that appear front-facing in projection.
    const Point2& p0 = pts[faces[i].idx[0]];
    const Point2& p1 = pts[faces[i].idx[1]];
    const Point2& p2 = pts[faces[i].idx[2]];
    faces[i].visible = IsFrontFace2D(p0, p1, p2);

    // diffuse light contribution depending on angle between normal and light direction
    float ndl = Dot3(n, lightDir);
    if (ndl < 0.0f) ndl = 0.0f;

    // specular
    Vec3 refl = Reflect3(
      { -lightDir.x, -lightDir.y, -lightDir.z },
      n
    );
    refl = Normalize3(refl);

    float rv = Dot3(refl, viewDir);
    if (rv < 0.0f) rv = 0.0f;
    float spec = powf(rv, 18.0f) * 0.85f;

    faces[i].shade = 0.75f + 0.95f * ndl + spec;
    if (faces[i].shade > 1.35f) faces[i].shade = 1.35f;
  }

  SortFacesByDepth(faces, 6);

  // draw
  for (int i = 0; i < 6; i++)
  {
    if (!faces[i].visible) continue;

    const Point2& p0 = pts[faces[i].idx[0]];
    const Point2& p1 = pts[faces[i].idx[1]];
    const Point2& p2 = pts[faces[i].idx[2]];
    const Point2& p3 = pts[faces[i].idx[3]];

    float brightness = faces[i].shade;
    float specular = 0.0f;

    // split highlight from main brightness
    if (brightness > 1.0f)
    {
      specular = brightness - 1.0f;
      brightness = 1.0f;
    }

    uint16_t fillColor = Shade565_Lit(faces[i].baseColor, brightness, specular);
    uint16_t edgeColor = Shade565_Lit(TFT_WHITE, 0.75f + 0.25f * brightness, specular * 0.5f);

    FillQuad(p0, p1, p2, p3, fillColor);
    DrawQuadBorder(p0, p1, p2, p3, edgeColor);
  }

  sprite.drawFastHLine(centerX - 3, centerY, 7, TFT_DARKGREY);
  sprite.drawFastVLine(centerX, centerY - 3, 7, TFT_DARKGREY);

  DrawFPSOnSprite();
  sprite.pushSprite(0, 0);

  AdvanceAngles(speed);
}


// ======================================================
// SETUP / LOOP
// ======================================================
void setup()
{
  // Initialize serial + LCD driver + sprite buffer.
  Serial.begin(115200);
  delay(200);

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);
  lcd.fillScreen(TFT_BLACK);

  sprite.setColorDepth(16);
  g_spriteReady = (sprite.createSprite(172, 320) != nullptr);

  g_fpsLastTickMs = millis();
}

void loop()
{
  // Render and rotate cube continuously, update FPS every 250ms.
  Render_3DBox(86, 160, 46.0f, 120.0f, 0.85f);

  g_fpsFrameCount++;
  uint32_t nowMs = millis();
  uint32_t dtMs = nowMs - g_fpsLastTickMs;

  if (dtMs >= 250)
  {
    g_fps = (1000.0f * (float)g_fpsFrameCount) / (float)dtMs;
    g_fpsFrameCount = 0;
    g_fpsLastTickMs = nowMs;

    Serial.printf("FPS = %.2f\n", g_fps);
  }
}