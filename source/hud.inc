#define UI_WIDTH 1280
#define UI_HEIGHT 720
#define ROWS 8
typedef uint32_t Color;
typedef struct { uint32_t *pixels; unsigned stride; } Surface;
typedef struct { uint32_t offset; int8_t left, top; uint8_t width, height, advance; } Glyph;

#include "ui_assets.inc"

enum { SMALL, BODY, LABEL, SUBTITLE, TITLE };
static const Glyph *const glyphs[] = { glyphs_0, glyphs_1, glyphs_2, glyphs_3, glyphs_4 };
static const uint8_t *const masks[] = { pixels_0, pixels_1, pixels_2, pixels_3, pixels_4 };
static const int line_height[] = { 21, 25, 29, 39, 55 };

#define RGB(r,g,b) (0xff000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))
static void fill(Surface s, int x, int y, int width, int height, Color color)
{
    for (int row = 0; row < height; ++row) {
        uint32_t *dest = s.pixels + (y + row) * s.stride + x;
        for (int col = 0; col < width; ++col)
            dest[col] = color;
    }
}

static void rounded(Surface s, int x, int y, int width, int height, Color color)
{
    static const int corners[] = { 7, 5, 3, 2, 2, 1, 1 };
    for (int row = 0; row < 7; ++row) {
        int inset = corners[row];
        fill(s, x + inset, y + row, width - inset * 2, 1, color);
        fill(s, x + inset, y + height - 1 - row, width - inset * 2, 1, color);
    }
    fill(s, x, y + 7, width, height - 14, color);
}

static void box(Surface s, int x, int y, int w, int h, Color edge, Color face)
{
    rounded(s, x, y, w, h, edge);
    rounded(s, x + 1, y + 1, w - 2, h - 2, face);
}

static const Glyph *next_glyph(int face, const char **cursor)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    unsigned c = *s++;
    if (c >= 0xc2 && c <= 0xf4) {
        int tail = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        c &= 0x7f >> (tail + 1);
        while (tail && (*s & 0xc0) == 0x80) {
            c = (c << 6) | (*s++ & 0x3f);
            --tail;
        }
        if (tail)
            c = '?';
    }
    *cursor = (const char *)s;
    unsigned index = '?' - 32;
    if (c >= 32 && c <= 126)
        index = c - 32;
    else if (c >= 160 && c <= 255)
        index = c - 65;
    else if (c == 0x2026)
        index = 191;
    else if (c == 0x2014)
        index = 192;
    else if (c == 0x2191 || c == 0x2193)
        index = 193 + (c == 0x2193);
    return &glyphs[face][index];
}

static int measure(int face, const char *text)
{
    int width = 0;
    while (*text)
        width += next_glyph(face, &text)->advance;
    return width;
}

static void draw_glyph(Surface s, int x, int y, int face, const Glyph *g, Color color)
{
    const uint8_t *data = masks[face] + g->offset;
    for (unsigned row = 0; row < g->height; ++row) {
        int gy = y + g->top + (int)row;
        if (gy < 0 || gy >= UI_HEIGHT)
            continue;
        for (unsigned col = 0; col < g->width; ++col) {
            int gx = x + g->left + (int)col;
            if (gx < 0 || gx >= UI_WIDTH)
                continue;
            unsigned n = row * g->width + col;
            unsigned a = (data[n / 2] >> (n % 2 ? 0 : 4)) & 15;
            if (!a)
                continue;
            uint32_t *pixel = &s.pixels[gy * s.stride + gx];
            uint32_t result = 0xff000000u;
            for (unsigned shift = 0; shift <= 16; shift += 8) {
                unsigned src = (color >> shift) & 255, dst = (*pixel >> shift) & 255;
                result |= ((src * a + dst * (15 - a) + 7) / 15) << shift;
            }
            *pixel = result;
        }
    }
}

static void text(Surface s, int x, int y, int width, int face, Color color, const char *value)
{
    const Glyph *ellipsis = &glyphs[face][191];
    bool clip = measure(face, value) > width;
    int limit = x + width - (clip ? ellipsis->advance : 0);
    while (*value) {
        const Glyph *g = next_glyph(face, &value);
        if (x + g->advance > limit)
            break;
        draw_glyph(s, x, y, face, g, color);
        x += g->advance;
    }
    if (clip)
        draw_glyph(s, x, y, face, ellipsis, color);
}

static void paragraph(Surface s, int x, int y, int width, int face, Color color,
                      const char *value, unsigned lines)
{
    while (*value && lines) {
        char line[256];
        const char *end = value, *space = NULL;
        int remaining = width;
        while (*end && *end != '\n') {
            const char *next = end;
            remaining -= next_glyph(face, &next)->advance;
            if (remaining < 0 || (size_t)(next - value) >= sizeof(line))
                break;
            if (*end == ' ')
                space = end;
            end = next;
        }
        if (lines == 1 || end == value) {
            text(s, x, y, width, face, color, value);
            return;
        }
        if (*end && *end != ' ' && *end != '\n' && space)
            end = space;
        size_t length = end - value;
        memcpy(line, value, length);
        line[length] = '\0';
        text(s, x, y, width, face, color, line);
        value = end;
        while (*value == ' ' || *value == '\n')
            ++value;
        y += line_height[face] + 7;
        --lines;
    }
}

static Framebuffer display;
static const Color canvas = RGB(13,16,26), shelf = RGB(23,27,43), tile = RGB(32,36,56);
static const Color edge = RGB(60,68,97), bright = RGB(243,245,255), quiet = RGB(180,189,215);
static const Color mint = RGB(117,239,196), active = RGB(35,61,64), violet = RGB(184,169,255);

static void right_text(Surface s, int right, int y, int width, int face, Color color, const char *value)
{
    int actual = measure(face, value);
    text(s, right - (actual < width ? actual : width), y, width, face, color, value);
}

static void key_hint(Surface s, int *x, const char *key, const char *label, bool primary)
{
    int width = measure(SMALL, key) + 20;
    box(s, *x, 666, width, 34, primary ? mint : edge, primary ? active : tile);
    text(s, *x + 10, 673, width - 12, SMALL, primary ? mint : bright, key);
    text(s, *x + width + 9, 673, 220, SMALL, primary ? bright : quiet, label);
    *x += width + 23 + measure(SMALL, label);
}

static Surface begin_hud(const char *section)
{
    u32 stride;
    uint32_t *pixels = framebufferBegin(&display, &stride);
    Surface s = {pixels, stride / sizeof *pixels};
    fill(s, 0, 0, UI_WIDTH, UI_HEIGHT, canvas);
    fill(s, 0, 0, UI_WIDTH, 104, shelf);
    fill(s, 0, 103, UI_WIDTH, 1, edge);
    fill(s, 0, 650, UI_WIDTH, 70, shelf);
    fill(s, 0, 650, UI_WIDTH, 1, edge);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            unsigned n = (y * 64 + x) * 3;
            s.pixels[(20 + y) * s.stride + 42 + x] =
                RGB(brand_icon[n], brand_icon[n + 1], brand_icon[n + 2]);
        }
    text(s, 122, 18, 400, SUBTITLE, bright, "Xfoil");
    text(s, 123, 61, 400, SMALL, violet, section);
    right_text(s, 1234, 25, 440, BODY, bright, "IndicaX");
    right_text(s, 1234, 59, 440, SMALL, quiet, "Xfoil " APP_VERSION);
    return s;
}

static void draw_task(const char *title, const char *message, int percent)
{
    Surface s = begin_hud("Transferências");
    box(s, 150, 195, 980, 315, edge, shelf);
    fill(s, 186, 234, 46, 3, mint);
    text(s, 186, 265, 905, SUBTITLE, bright, title);
    paragraph(s, 186, 326, 905, BODY, quiet, message, 2);
    if (percent >= 0) {
        char label[24];
        snprintf(label, sizeof label, "%d%%", percent);
        fill(s, 186, 439, 809, 9, edge);
        fill(s, 186, 439, percent * 809 / 100, 9, mint);
        right_text(s, 1089, 423, 82, LABEL, mint, label);
    }
    framebufferEnd(&display);
}

static void draw_details(Surface s, const Item *item, bool ready)
{
    box(s, 112, 129, 1056, 461, edge, shelf);
    fill(s, 149, 165, 45, 3, mint);
    text(s, 149, 191, 971, SUBTITLE, bright, item->title);
    text(s, 149, 253, 970, SMALL, quiet, "Jogo");
    text(s, 149, 279, 970, LABEL, bright, item->game[0] ? item->game : item->title);
    text(s, 149, 330, 970, SMALL, quiet, "Versão do jogo");
    paragraph(s, 149, 358, 970, LABEL, violet, item->requirement[0] ? item->requirement : "Não informada", 2);
    char size[48];
    snprintf(size, sizeof size, "%.1f MiB", (double)item->size / 1048576.0);
    text(s, 149, 444, 810, BODY, bright, item->file);
    right_text(s, 1120, 444, 140, BODY, mint, size);
    if (ready) {
        text(s, 149, 499, 970, BODY, bright, "No DBI, navegue pelo microSD até a pasta abaixo.");
        text(s, 149, 534, 970, BODY, mint, "switch/xfoil/downloads/");
    }
}

static void draw_catalog(Surface s, size_t server, size_t selected, const size_t *visible, size_t matches)
{
    char meta[128];
    snprintf(meta, sizeof meta, "Servidor %zu de 4   ·   %s", server + 1,
             expire_downloads ? "Expiração em 8 horas" : "Expiração desligada");
    text(s, 43, 115, 900, SUBTITLE, bright, "Baixar do catálogo");
    text(s, 44, 162, 755, SMALL, quiet, *sources[server] ? sources[server] : "Sem servidor configurado");
    right_text(s, 1235, 119, 430, SMALL, violet, meta);
    snprintf(meta, sizeof meta, "%zu %s%s", matches, *filter ? "resultados · " : "arquivos", filter);
    right_text(s, 1235, 162, 430, SMALL, quiet, meta);
    box(s, 42, 197, 1196, 407, edge, shelf);
    if (!matches) {
        text(s, 67, 232, 1135, LABEL, bright, "Nenhum arquivo para mostrar");
        text(s, 67, 275, 1135, BODY, quiet, *sources[server] ?
             "Carregue o catálogo com Y ou ajuste a busca." :
             "Use X para informar o endereço HTTPS do catálogo.");
        return;
    }
    text(s, 75, 207, 650, SMALL, quiet, "Arquivo");
    text(s, 816, 207, 190, SMALL, quiet, "Formato");
    right_text(s, 1202, 207, 140, SMALL, quiet, "Tamanho");
    size_t first = selected / ROWS * ROWS;
    for (size_t i = first; i < matches && i < first + ROWS; ++i) {
        const Item *item = &items[visible[i]];
        int y = 236 + (int)(i - first) * 44;
        bool focus = i == selected;
        box(s, 57, y, 1166, 40, focus ? mint : shelf, focus ? active : tile);
        if (focus) fill(s, 60, y + 8, 3, 24, mint);
        text(s, 76, y + 7, 712, BODY, focus ? mint : bright, item->title);
        const char *ext = strrchr(item->file, '.');
        if (ext) text(s, 816, y + 8, 160, SMALL, quiet, ext + 1);
        char size[45];
        snprintf(size, sizeof size, "%.1f MiB", (double)item->size / 1048576.0);
        right_text(s, 1202, y + 8, 141, SMALL, quiet, size);
    }
}

static void draw_hud(size_t server, size_t selected, const size_t *visible, size_t matches,
                     bool confirm, bool ready)
{
    Surface s = begin_hud(confirm ? "Detalhes do arquivo" : "Catálogo");
    int x = 42;
    if (confirm && matches) {
        draw_details(s, &items[visible[selected]], ready);
        key_hint(s, &x, "A", ready ? "Abrir DBI" : "Baixar e verificar", true);
        key_hint(s, &x, "B", "Voltar", false);
    } else {
        draw_catalog(s, server, selected, visible, matches);
        if (matches) key_hint(s, &x, "A", "Detalhes", true);
        key_hint(s, &x, "Y", "Carregar", !matches);
        key_hint(s, &x, "X", "Editar", false);
        key_hint(s, &x, "R", "Servidor", false);
        key_hint(s, &x, "L", "Buscar", false);
        key_hint(s, &x, "ZR", "Expiração", false);
    }
    x = 1126;
    key_hint(s, &x, "+", "Sair", false);
    text(s, 44, 619, 1170, SMALL, quiet, status);
    framebufferEnd(&display);
}
