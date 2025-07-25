#include "vendor/glad/glad.h"
#include <GLFW/glfw3.h>
#include "vendor/stb/stb_image.h"
#include "vendor/stb/stb_image_write.h"
#include "base/log.h"
#include "base/math.h"
#include "base/string.h"
#include "os/time.h"
#include "base/map.h"
#include "os/fs.h"
#include "ui/font.h"

istruct (Ui);
static Void ui_init (Mem *, Mem *);
static Void ui_frame (F32 dt);
Ui *ui;

// =============================================================================
// Glfw and opengl layer:
// =============================================================================
#define VERTEX_MAX_BATCH_SIZE 2400

ienum (EventTag, U8) {
    EVENT_DUMMY,
    EVENT_EATEN,
    EVENT_WINDOW_SIZE,
    EVENT_MOUSE_MOVE,
    EVENT_SCROLL,
    EVENT_KEY_PRESS,
    EVENT_KEY_RELEASE,
};

istruct (Event) {
    EventTag tag;
    F64 x;
    F64 y;
    Int key;
    Int mods;
    Int scancode;
};

istruct (RectAttributes) {
    Vec4 color;
    Vec4 color2; // If x = -1, no gradient.
    Vec2 top_left;
    Vec2 bottom_right;
    Vec4 radius;
    F32  edge_softness;
    Vec4 border_color;
    Vec4 border_widths;
    Vec4 inset_shadow_color;
    Vec4 outset_shadow_color;
    F32  outset_shadow_width;
    F32  inset_shadow_width;
    Vec2 shadow_offsets;
    Vec4 texture_rect;
    Vec4 text_color;
    F32 text_is_grayscale;
};

istruct (Vertex) {
    Vec2 position;
    Vec4 color;
    Vec2 top_left;
    Vec2 bottom_right;
    Vec4 radius;
    F32 edge_softness;
    Vec4 border_color;
    Vec4 border_widths;
    Vec4 inset_shadow_color;
    Vec4 outset_shadow_color;
    F32 outset_shadow_width;
    F32 inset_shadow_width;
    Vec2 shadow_offsets;
    Vec2 uv;
    Vec4 text_color;
    F32 text_is_grayscale;
};

array_typedef(Vertex, Vertex);
array_typedef(Event, Event);

Arena *parena;
Arena *farena; // Cleared each frame.

GLFWwindow *window;
Int win_width  = 800;
Int win_height = 600;

#define BLUR_SHRINK 5
U32 blur_shader;
U32 blur_VBO, blur_VAO;
Array(struct { Vec2 pos; }) blur_vertices;
U32 blur_buffer1;
U32 blur_buffer2;
U32 blur_tex1;
U32 blur_tex2;

ArrayVertex vertices;
ArrayEvent events;

U32 rect_shader;
U32 VBO, VAO;
Mat4 projection;
U32 framebuffer;
U32 framebuffer_tex;

U32 screen_shader;
U32 screen_VBO, screen_VAO;
Array(struct { Vec2 pos; Vec2 tex; }) screen_vertices;

F32 dt;
F32 prev_frame;
F32 current_frame;
U64 frame_count;
F32 first_counted_frame;

static U32 framebuffer_new (U32 *out_texture, Bool only_color_attach, U32 w, U32 h);

static Void set_bool  (U32 p, CString name, Bool v) { glUniform1i(glGetUniformLocation(p, name), cast(Int, v)); }
static Void set_int   (U32 p, CString name, Int v)  { glUniform1i(glGetUniformLocation(p, name), v); }
static Void set_float (U32 p, CString name, F32 v)  { glUniform1f(glGetUniformLocation(p, name), v); }
static Void set_vec2  (U32 p, CString name, Vec2 v) { glUniform2f(glGetUniformLocation(p, name), v.x, v.y); }
static Void set_vec4  (U32 p, CString name, Vec4 v) { glUniform4f(glGetUniformLocation(p, name), v.x, v.y, v.z, v.w); }
static Void set_mat4  (U32 p, CString name, Mat4 m) { glUniformMatrix4fv(glGetUniformLocation(p, name), 1, GL_FALSE, cast(F32*, &m)); }

#define ATTR(T, OFFSET, LEN, NAME) ({\
    glVertexAttribPointer(OFFSET, LEN, GL_FLOAT, GL_FALSE, sizeof(T), cast(Void*, offsetof(T, NAME)));\
    glEnableVertexAttribArray(OFFSET);\
})

Noreturn static Void error () {
    log_scope_end_all();
    panic();
}

Noreturn Fmt(1, 2) static Void error_fmt (CString fmt, ...) {
    log_msg(m, LOG_ERROR, "UI", 1);
    astr_push_fmt_vam(m, fmt);
    astr_push_byte(m, '\n');
    error();
}

static Void update_projection () {
    F32 h = cast(F32, win_height);
    F32 w = cast(F32, win_width);
    projection = mat_ortho(0, w, 0, h, -1.f, 1.f);
}

static Void framebuffer_size_callback (GLFWwindow *win, Int width, Int height) {
    win_width = width;
    win_height = height;
    update_projection();
    glViewport(0, 0, width, height);
    framebuffer = framebuffer_new(&framebuffer_tex, 1,   win_width, win_height);
    blur_buffer1 = framebuffer_new(&blur_tex1, 1, floor(win_width/BLUR_SHRINK), floor(win_height/BLUR_SHRINK));
    blur_buffer2 = framebuffer_new(&blur_tex2, 1, floor(win_width/BLUR_SHRINK), floor(win_height/BLUR_SHRINK));
    glScissor(0, 0, width, height);
    Auto e = array_push_slot(&events);
    e->tag = EVENT_WINDOW_SIZE;
}

Void mouse_scroll_callback (GLFWwindow *win, F64 x, F64 y) {
    Auto e = array_push_slot(&events);
    e->tag = EVENT_SCROLL;
    e->x = x;
    e->y = y;
}

Void mouse_move_callback (GLFWwindow *win, F64 x, F64 y) {
    Auto e = array_push_slot(&events);
    e->tag = EVENT_MOUSE_MOVE;
    e->x = x;
    e->y = y;
}

Void mouse_click_callback (GLFWwindow *win, Int button, Int action, Int mods) {
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return;
    Auto e = array_push_slot(&events);
    e->tag = action == GLFW_PRESS ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;
    e->key = button;
    e->mods = mods;
}

Void key_callback (GLFWwindow* window, Int key, Int scancode, Int action, Int mods) {
    Auto e = array_push_slot(&events);
    e->tag = action == GLFW_RELEASE ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS;
    e->key = key;
    e->mods = mods;
    e->scancode = scancode;
}

Void write_texture_to_png (CString filepath, U32 texture, U32 w, U32 h, Bool flip) {
    tmem_new(tm);
    Auto data = mem_alloc(tm, Void, .size=(4*w*h));

    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_flip_vertically_on_write(flip);
    Int result = stbi_write_png(filepath, w, h, 4, data, w*4);
    assert_always(result);
}

U32 load_texture (CString filepath) {
    U32 id; glGenTextures(1, &id);

    glBindTexture(GL_TEXTURE_2D, id);

    glTextureParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTextureParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_set_flip_vertically_on_load(true);

    Int w, h, n; U8 *data = stbi_load(filepath, &w, &h, &n, 0);
    if (! data) error_fmt("Couldn't load image from file: %s\n", filepath);

    Int fmt = (n == 3) ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(data);
    return id;
}

static U32 framebuffer_new (U32 *out_texture, Bool only_color_attach, U32 w, U32 h) {
    U32 r;
    glGenFramebuffers(1, &r);
    glBindFramebuffer(GL_FRAMEBUFFER, r);

    U32 texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (out_texture) *out_texture = texture;

    if (! only_color_attach) {
        U32 rbo;
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
    }

    assert_always(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return r;
}

static U32 shader_compile (GLenum type, String filepath) {
    tmem_new(tm);

    String source = fs_read_entire_file(tm, filepath, 0);
    if (! source.data) error_fmt("Unable to read file: %.*s\n", STR(filepath));

    U32 shader = glCreateShader(type);

    glShaderSource(shader, 1, cast(const GLchar**, &source.data), 0);
    glCompileShader(shader);

    Int success; glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (! success) {
        log_msg(msg, LOG_ERROR, "", 1);
        Int count; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &count);
        astr_push_fmt(msg, "Shader compilation error: %.*s\n  ", STR(filepath));
        U32 offset = msg->count;
        array_increase_count(msg, cast(U32, count), false);
        glGetShaderInfoLog(shader, count, 0, msg->data + offset);
        msg->count--; // Get rid of the NUL byte...
        error();
    }

    return shader;
}

static U32 shader_new (CString vshader_path, CString fshader_path) {
    U32 id      = glCreateProgram();
    U32 vshader = shader_compile(GL_VERTEX_SHADER, str(vshader_path));
    U32 fshader = shader_compile(GL_FRAGMENT_SHADER, str(fshader_path));

    glAttachShader(id, vshader);
    glAttachShader(id, fshader);
    glLinkProgram(id);

    Int success; glGetProgramiv(id, GL_LINK_STATUS, &success);
    if (! success) {
        log_msg(msg, LOG_ERROR, "", 1);
        Int count; glGetProgramiv(id, GL_INFO_LOG_LENGTH, &count);
        astr_push_cstr(msg, "Shader prog link error.\n  ");
        U32 offset = msg->count;
        array_increase_count(msg, cast(U32, count), false);
        glGetProgramInfoLog(id, count, 0, msg->data + offset);
        msg->count--; // Get rid of the NUL byte...
        error();
    }

    glDeleteShader(vshader);
    glDeleteShader(fshader);

    return id;
}

static Void flush_vertices () {
    glBindVertexArray(VAO);
    glUseProgram(rect_shader);
    set_mat4(rect_shader, "projection", projection);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    ATTR(Vertex, 0, 2, position);
    ATTR(Vertex, 1, 4, color);
    ATTR(Vertex, 2, 2, top_left);
    ATTR(Vertex, 3, 2, bottom_right);
    ATTR(Vertex, 4, 4, radius);
    ATTR(Vertex, 5, 1, edge_softness);
    ATTR(Vertex, 6, 4, border_color);
    ATTR(Vertex, 7, 4, border_widths);
    ATTR(Vertex, 8, 4, inset_shadow_color);
    ATTR(Vertex, 9, 4, outset_shadow_color);
    ATTR(Vertex, 10, 1, outset_shadow_width);
    ATTR(Vertex, 11, 1, inset_shadow_width);
    ATTR(Vertex, 12, 2, shadow_offsets);
    ATTR(Vertex, 13, 2, uv);
    ATTR(Vertex, 14, 4, text_color);
    ATTR(Vertex, 15, 1, text_is_grayscale);

    glBufferData(GL_ARRAY_BUFFER, array_size(&vertices), vertices.data, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, vertices.count);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    vertices.count = 0;
}

static Vertex *reserve_vertices (U32 n) {
    if (vertices.count + n >= VERTEX_MAX_BATCH_SIZE) flush_vertices();
    SliceVertex slice;
    array_increase_count_o(&vertices, n, false, &slice);
    return slice.data;
}

static Void draw_rect_vertex (Vertex *v, Vec2 pos, Vec2 uv, Vec4 color, RectAttributes *a) {
    v->position            = pos;
    v->color               = color;
    v->top_left            = a->top_left;
    v->bottom_right        = a->bottom_right;
    v->radius              = a->radius;
    v->edge_softness       = a->edge_softness;
    v->border_color        = a->border_color;
    v->border_widths       = a->border_widths;
    v->inset_shadow_color  = a->inset_shadow_color;
    v->outset_shadow_color = a->outset_shadow_color;
    v->outset_shadow_width = a->outset_shadow_width;
    v->inset_shadow_width  = a->inset_shadow_width;
    v->shadow_offsets      = a->shadow_offsets;
    v->uv                  = uv;
    v->text_color          = a->text_color;
    v->text_is_grayscale   = a->text_is_grayscale;
}

#define draw_rect(...) draw_rect_fn(&(RectAttributes){__VA_ARGS__})

static Void draw_rect_fn (RectAttributes *a) {
    Vertex *p = reserve_vertices(6);

    if (a->color2.x == -1.0) a->color2 = a->color;

    // We make the rect slightly bigger because the fragment
    // shader will shrink it to make room for the drop shadow.
    a->top_left.x     -= 2*a->outset_shadow_width + 2*a->edge_softness;
    a->top_left.y     -= 2*a->outset_shadow_width + 2*a->edge_softness;
    a->bottom_right.x += 2*a->outset_shadow_width + 2*a->edge_softness;
    a->bottom_right.y += 2*a->outset_shadow_width + 2*a->edge_softness;

    a->top_left.y = win_height - a->top_left.y;
    a->bottom_right.y = win_height - a->bottom_right.y;

    Vec2 bottom_left = vec2(a->top_left.x, a->bottom_right.y);
    Vec2 top_right   = vec2(a->bottom_right.x, a->top_left.y);

    Vec4 tr = a->texture_rect;

    draw_rect_vertex(&p[0], a->top_left, vec2(tr.x, tr.y), a->color, a);
    draw_rect_vertex(&p[1], bottom_left, vec2(tr.x, tr.y+tr.w), a->color2, a);
    draw_rect_vertex(&p[2], a->bottom_right, vec2(tr.x+tr.z, tr.y+tr.w), a->color2, a);
    draw_rect_vertex(&p[3], a->bottom_right, vec2(tr.x+tr.z, tr.y+tr.w), a->color2, a);
    draw_rect_vertex(&p[4], top_right, vec2(tr.x+tr.z, tr.y), a->color, a);
    draw_rect_vertex(&p[5], a->top_left, vec2(tr.x, tr.y), a->color, a);
}

Void ui_test () {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(win_width, win_height, "Mykron", 0, 0);
    if (! window) { glfwTerminate(); return; }

    glfwMakeContextCurrent(window);

    if (! gladLoadGLLoader(cast(GLADloadproc, glfwGetProcAddress))) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetScrollCallback(window, mouse_scroll_callback);
    glfwSetCursorPosCallback(window, mouse_move_callback);
    glfwSetMouseButtonCallback(window, mouse_click_callback);

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    parena = arena_new(mem_root, 1*MB);
    farena = arena_new(mem_root, 1*MB);

    framebuffer   = framebuffer_new(&framebuffer_tex, 1, win_width, win_height);
    blur_buffer1  = framebuffer_new(&blur_tex1, 1, floor(win_width/BLUR_SHRINK), floor(win_height/BLUR_SHRINK));
    blur_buffer2  = framebuffer_new(&blur_tex2, 1, floor(win_width/BLUR_SHRINK), floor(win_height/BLUR_SHRINK));
    rect_shader   = shader_new("src/ui/rect_vs.glsl", "src/ui/rect_fs.glsl");
    screen_shader = shader_new("src/ui/screen_vs.glsl", "src/ui/screen_fs.glsl");
    blur_shader   = shader_new("src/ui/blur_vs.glsl", "src/ui/blur_fs.glsl");

    { // Screen quad init:
        array_init(&screen_vertices, parena);
        array_push_lit(&screen_vertices, .pos={-1.0f,  1.0f},  .tex={0.0f, 1.0f});
        array_push_lit(&screen_vertices, .pos={-1.0f, -1.0f},  .tex={0.0f, 0.0f});
        array_push_lit(&screen_vertices, .pos={ 1.0f, -1.0f},  .tex={1.0f, 0.0f});
        array_push_lit(&screen_vertices, .pos={-1.0f,  1.0f},  .tex={0.0f, 1.0f});
        array_push_lit(&screen_vertices, .pos={ 1.0f, -1.0f},  .tex={1.0f, 0.0f});
        array_push_lit(&screen_vertices, .pos={ 1.0f,  1.0f},  .tex={1.0f, 1.0f});

        glGenVertexArrays(1, &screen_VAO);
        glGenBuffers(1, &screen_VBO);
        glBindVertexArray(screen_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, screen_VBO);
        glBufferData(GL_ARRAY_BUFFER, array_size(&screen_vertices), screen_vertices.data, GL_STATIC_DRAW);
        ATTR(AElem(&screen_vertices), 0, 2, pos);
        ATTR(AElem(&screen_vertices), 1, 2, tex);

        glUseProgram(screen_shader);
        set_int(screen_shader, "tex", 0);
    }

    array_init(&blur_vertices, parena);
    glGenVertexArrays(1, &blur_VAO);
    glBindVertexArray(blur_VAO);
    glGenBuffers(1, &blur_VBO);
    ATTR(AElem(&blur_vertices), 0, 2, pos);
    glBindVertexArray(0);

    array_init(&vertices, parena);
    array_init(&events, parena);
    ui_init(cast(Mem*, parena), cast(Mem*, farena));
    update_projection();

    dt                  = 0;
    frame_count         = 0;
    current_frame       = glfwGetTime();
    prev_frame          = current_frame - 0.16f;
    first_counted_frame = current_frame;

    while(! glfwWindowShouldClose(window)) {
        current_frame = glfwGetTime();
        dt            = current_frame - prev_frame;
        prev_frame    = current_frame;
        frame_count++;

        log_scope(ls, 1);
        arena_pop_all(farena);

        #if 0
        if (current_frame - first_counted_frame >= 0.1) {
            tmem_new(tm);
            String title = astr_fmt(tm, "fps: %lu%c", cast(U64, round(frame_count/(current_frame - first_counted_frame))), 0);
            glfwSetWindowTitle(window, title.data);
            frame_count = 0;
            first_counted_frame = current_frame;
        }
        #endif

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (events.count == 0) array_push_lit(&events, .tag=EVENT_DUMMY);
        ui_frame(dt);
        events.count = 0;
        if (vertices.count) flush_vertices();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(screen_shader);
        glBindVertexArray(screen_VAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, framebuffer_tex);
        glDrawArrays(GL_TRIANGLES, 0, screen_vertices.count);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteProgram(rect_shader);
    glDeleteProgram(screen_shader);
    glfwTerminate();
    arena_destroy(parena);
    arena_destroy(farena);
}

// =============================================================================
// UI layer:
// =============================================================================
typedef U64 UiKey;

ienum (UiSizeTag, U8) {
    UI_SIZE_TEXT,
    UI_SIZE_PIXELS,
    UI_SIZE_PCT_PARENT,
    UI_SIZE_CHILDREN_SUM,
};

istruct (UiSize) {
    UiSizeTag tag;
    F32 value;
    F32 strictness;
};

ienum (UiAlign, U32) {
    UI_ALIGN_START,
    UI_ALIGN_MIDDLE,
    UI_ALIGN_END,
};

iunion (UiBoxSize) {
    struct { UiSize width, height; };
    UiSize v[2];
};

ienum (UiAxis, U8) {
    UI_AXIS_HORIZONTAL,
    UI_AXIS_VERTICAL,
};

assert_static(UI_AXIS_HORIZONTAL == 0);
assert_static(UI_AXIS_VERTICAL == 1);

ienum (UiStyleAttribute, U32) {
    UI_WIDTH,
    UI_HEIGHT,
    UI_AXIS,
    UI_BG_COLOR,
    UI_BG_COLOR2,
    UI_TEXT_COLOR,
    UI_RADIUS,
    UI_PADDING,
    UI_SPACING,
    UI_ALIGN_X,
    UI_ALIGN_Y,
    UI_FLOAT_X,
    UI_FLOAT_Y,
    UI_OVERFLOW_X,
    UI_OVERFLOW_Y,
    UI_EDGE_SOFTNESS,
    UI_BORDER_COLOR,
    UI_BORDER_WIDTHS,
    UI_INSET_SHADOW_COLOR,
    UI_OUTSET_SHADOW_COLOR,
    UI_INSET_SHADOW_WIDTH,
    UI_OUTSET_SHADOW_WIDTH,
    UI_SHADOW_OFFSETS,
    UI_BLUR_RADIUS,
    UI_ANIMATION,
    UI_ANIMATION_TIME,
    UI_ATTRIBUTE_COUNT,
};

fenum (UiStyleMask, U32) {
    UI_MASK_WIDTH               = 1 << UI_WIDTH,
    UI_MASK_HEIGHT              = 1 << UI_HEIGHT,
    UI_MASK_AXIS                = 1 << UI_AXIS,
    UI_MASK_BG_COLOR            = 1 << UI_BG_COLOR,
    UI_MASK_BG_COLOR2           = 1 << UI_BG_COLOR2,
    UI_MASK_TEXT_COLOR          = 1 << UI_TEXT_COLOR,
    UI_MASK_RADIUS              = 1 << UI_RADIUS,
    UI_MASK_PADDING             = 1 << UI_PADDING,
    UI_MASK_SPACING             = 1 << UI_SPACING,
    UI_MASK_ALIGN_X             = 1 << UI_ALIGN_X,
    UI_MASK_ALIGN_Y             = 1 << UI_ALIGN_Y,
    UI_MASK_FLOAT_X             = 1 << UI_FLOAT_X,
    UI_MASK_FLOAT_Y             = 1 << UI_FLOAT_Y,
    UI_MASK_OVERFLOW_X          = 1 << UI_OVERFLOW_X,
    UI_MASK_OVERFLOW_Y          = 1 << UI_OVERFLOW_Y,
    UI_MASK_EDGE_SOFTNESS       = 1 << UI_EDGE_SOFTNESS,
    UI_MASK_BORDER_COLOR        = 1 << UI_BORDER_COLOR,
    UI_MASK_BORDER_WIDTHS       = 1 << UI_BORDER_WIDTHS,
    UI_MASK_INSET_SHADOW_COLOR  = 1 << UI_INSET_SHADOW_COLOR,
    UI_MASK_OUTSET_SHADOW_COLOR = 1 << UI_OUTSET_SHADOW_COLOR,
    UI_MASK_INSET_SHADOW_WIDTH  = 1 << UI_INSET_SHADOW_WIDTH,
    UI_MASK_OUTSET_SHADOW_WIDTH = 1 << UI_OUTSET_SHADOW_WIDTH,
    UI_MASK_SHADOW_OFFSETS      = 1 << UI_SHADOW_OFFSETS,
    UI_MASK_BLUR_RADIUS         = 1 << UI_BLUR_RADIUS,
    UI_MASK_ANIMATION           = 1 << UI_ANIMATION,
    UI_MASK_ANIMATION_TIME      = 1 << UI_ANIMATION_TIME,
};

istruct (UiStyle) {
    UiBoxSize size;
    UiAxis axis;
    Vec4 bg_color;
    Vec4 bg_color2; // If x = -1, no gradient.
    Vec4 text_color;
    Vec4 radius;
    Vec2 padding;
    F32  spacing;
    UiAlign align[2];
    F32  edge_softness;
    F32 floating[2]; // If NAN no floating.
    U32 overflow[2];
    Vec4 border_color;
    Vec4 border_widths;
    Vec4 inset_shadow_color;
    Vec4 outset_shadow_color;
    F32  inset_shadow_width;
    F32  outset_shadow_width;
    Vec2 shadow_offsets;
    F32  blur_radius; // 0 means no background blur.
    UiStyleMask animation_mask;
    F32 animation_time;
};

ienum (UiPatternTag, U8) {
    UI_PATTERN_PATH,
    UI_PATTERN_AND,
    UI_PATTERN_ANY,
    UI_PATTERN_ID,
    UI_PATTERN_TAG,
    UI_PATTERN_IS_ODD,
    UI_PATTERN_IS_EVEN,
    UI_PATTERN_IS_FIRST,
    UI_PATTERN_IS_LAST,
};

istruct (UiSpecificity) {
    U32 id;
    U32 tag;
};

istruct (UiPattern) {
    UiPatternTag tag;
    String string;
    UiSpecificity specificity;
    Array(UiPattern*) patterns;
};

istruct (UiStyleRule) {
    UiStyle *style;
    UiPattern *pattern;
    UiStyleMask mask;
};

istruct (UiBox);
array_typedef(UiBox*, UiBox);
array_typedef(UiPattern*, UiPattern);
array_typedef(UiStyleRule, UiStyleRule);
array_typedef(UiSpecificity, UiSpecificity);

istruct (UiSignal) {
    Bool hovered;
    Bool pressed;
    Bool clicked;
    Bool focused;
};

istruct (UiRect) {
    union { struct { F32 x, y; }; Vec2 top_left; };
    union { struct { F32 w, h; }; F32 size[2]; };
};

fenum (UiBoxFlags, U8) {
    UI_BOX_REACTIVE      = flag(0),
    UI_BOX_CAN_FOCUS     = flag(1),
    UI_BOX_INVISIBLE     = flag(2),
    UI_BOX_CLIPPING      = flag(3),
    UI_BOX_CLICK_THROUGH = flag(4),
    UI_BOX_DRAW_TEXT     = flag(5),
};

istruct (UiBox) {
    UiBox *parent;
    ArrayUiBox children;
    UiStyle style;
    UiStyle next_style;
    ArrayUiStyleRule style_rules;
    ArrayString tags;
    UiSignal signal;
    String label;
    UiKey key;
    UiBoxFlags flags;
    U8 gc_flag;
    U64 scratch;
    UiRect rect;
    UiRect text_rect;

    // The x/y components of this field are set independently
    // by the user build code for the purpose of scrolling the
    // content. The w/h components are set by the layout code.
    UiRect content;
};

istruct (Ui) {
    Mem *mem;
    Mem *frame_mem;
    U8 gc_flag;
    Event *event;
    Vec2 mouse_dt;
    Vec2 mouse;
    Map(U32, U8) pressed_keys;
    F32 dt;
    UiBox *root;
    UiBox *active;
    UiBox *hovered;
    UiBox *focused;
    U64 focus_idx;
    ArrayUiBox depth_first;
    ArrayUiBox free_boxes;
    ArrayUiBox box_stack;
    Map(UiKey, UiBox*) box_cache;
    Array(UiRect) clip_stack;
    UiStyleRule *current_style_rule;
    GlyphCache *glyph_cache;
};

static Void ui_tag (CString tag);

UiStyle default_box_style = {
    .size.width     = {UI_SIZE_CHILDREN_SUM, 0, 0},
    .size.height    = {UI_SIZE_CHILDREN_SUM, 0, 0},
    .bg_color2      = {-1},
    .text_color     = {1, 1, 1, .8},
    .edge_softness  = 1.0,
    .floating[0]    = NAN,
    .floating[1]    = NAN,
    .animation_time = .15,
};

static Bool is_key_pressed (Int key) {
    U8 val; Bool pressed = map_get(&ui->pressed_keys, key, &val);
    return pressed;
}

static Bool within_box (UiRect r, Vec2 p) {
    return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

static UiRect compute_rect_intersect (UiRect r0, UiRect r1) {
    F32 x0 = max(r0.x, r1.x);
    F32 y0 = max(r0.y, r1.y);
    F32 x1 = min(r0.x + r0.w, r1.x + r1.w);
    F32 y1 = min(r0.y + r0.h, r1.y + r1.h);
    return (UiRect){ x0, y0, max(0, x1 - x0), max(0, y1 - y0) };
}

static Void compute_signals (UiBox *box) {
    UiSignal *sig = &box->signal;
    *sig = (UiSignal){};

    if (! (box->flags & UI_BOX_REACTIVE)) return;

    sig->focused = (box == ui->focused);
    sig->clicked = sig->focused && (ui->event->tag == EVENT_KEY_PRESS) && (ui->event->key == GLFW_KEY_ENTER);

    Bool hovered = false;
    for (UiBox *b = ui->hovered; b; b = b->parent) {
        if (b == box) {
            UiRect intersection = compute_rect_intersect(box->rect, array_get_last(&ui->clip_stack));
            hovered = within_box(intersection, ui->mouse);
            break;
        }
    }

    if (! ui->active) {
        sig->hovered = hovered;

        if (ui->hovered == box && (ui->event->tag == EVENT_KEY_PRESS) && (ui->event->key == GLFW_MOUSE_BUTTON_LEFT)) {
            ui->active = box;
            sig->pressed = true;
        }
    } else if (ui->active == box) {
        sig->hovered = hovered;
        sig->pressed = false;

        if ((ui->event->tag == EVENT_KEY_RELEASE) && (ui->event->key == GLFW_MOUSE_BUTTON_LEFT)) {
            ui->active = 0;
            if (hovered) sig->clicked = true;
        } else {
            sig->hovered = hovered;
            sig->pressed = true;
        }
    }
}

static Void ui_init (Mem *mem, Mem *frame_mem) {
    ui = mem_new(mem, Ui);
    ui->mem = mem;
    ui->frame_mem = frame_mem;
    array_init(&ui->free_boxes, ui->mem);
    array_init(&ui->box_stack, ui->mem);
    array_init(&ui->clip_stack, ui->mem);
    array_init(&ui->depth_first, ui->mem);
    map_init(&ui->box_cache, mem);
    map_init(&ui->pressed_keys, mem);
    array_push_lit(&ui->clip_stack, .w=win_width, .h=win_height);
    ui->glyph_cache = glyph_cache_new(mem, 64, 16);
}

static UiKey ui_build_key (String string) {
    UiBox *parent = array_try_get_last(&ui->box_stack);
    U64 seed = parent ? parent->key : 0;
    return str_hash_seed(string, seed);
}

static Void ui_push_parent (UiBox *box) { array_push(&ui->box_stack, box); }
static Void ui_pop_parent  ()           { array_pop(&ui->box_stack); }
static Void ui_pop_parent_ (Void *)     { array_pop(&ui->box_stack); }

#define ui_parent(...)\
    ui_push_parent(__VA_ARGS__);\
    if (cleanup(ui_pop_parent_) U8 _; 1)

// The label is copied into per-frame memory, so
// no need to worry about lifetime issues.
static UiBox *ui_box_push_str (UiBoxFlags flags, String label) {
    UiKey key  = ui_build_key(label);
    UiBox *box = map_get_ptr(&ui->box_cache, key);

    if (box) {
        if (box->gc_flag == ui->gc_flag) error_fmt("UiBox label hash collision: [%.*s] vs [%.*s].", STR(box->label), STR(label));
        box->parent = 0;
        box->tags.count = 0;
        box->children.count = 0;
        box->style_rules.count = 0;
    } else if (ui->free_boxes.count) {
        box = array_pop(&ui->free_boxes);
        box->parent = 0;
        box->tags.count = 0;
        box->children.count = 0;
        box->style_rules.count = 0;
        box->style = default_box_style;
        box->rect = (UiRect){};
        box->text_rect = (UiRect){};
        box->content = (UiRect){};
        box->scratch = 0;
        map_add(&ui->box_cache, key, box);
    } else {
        box = mem_new(ui->mem, UiBox);
        array_init(&box->children, ui->mem);
        array_init(&box->style_rules, ui->mem);
        array_init(&box->tags, ui->mem);
        box->style = default_box_style;
        map_add(&ui->box_cache, key, box);
    }

    box->next_style = default_box_style;
    array_push(&ui->depth_first, box);
    box->label = str_copy(ui->frame_mem, label);
    box->key = key;
    box->gc_flag = ui->gc_flag;
    box->flags = flags;
    Auto parent = array_try_get_last(&ui->box_stack);
    if (parent) array_push(&parent->children, box);
    box->parent = parent;
    ui_push_parent(box);
    compute_signals(box);
    if (box->signal.focused) ui_tag("focus");
    if (box->signal.hovered) ui_tag("hover");
    if (box->signal.pressed) ui_tag("press");
    return box;
}

static UiBox *ui_box_push_fmt (UiBoxFlags flags, CString fmt, ...) {
    tmem_new(tm);
    AString a = astr_new(tm);
    astr_push_fmt_vam(&a, fmt);
    return ui_box_push_str(flags, astr_to_str(&a));
}

static UiBox *ui_box_push (UiBoxFlags flags, CString label) {
    return ui_box_push_str(flags, str(label));
}

#define ui_box(...)     ui_box_push(__VA_ARGS__);     if (cleanup(ui_pop_parent_) U8 _; 1)
#define ui_box_str(...) ui_box_push_str(__VA_ARGS__); if (cleanup(ui_pop_parent_) U8 _; 1)
#define ui_box_fmt(...) ui_box_push_fmt(__VA_ARGS__); if (cleanup(ui_pop_parent_) U8 _; 1)

static UiRect ui_push_clip_rect (UiRect rect) {
    UiRect intersection = compute_rect_intersect(rect, array_get_last(&ui->clip_stack));
    array_push(&ui->clip_stack, intersection);
    return intersection;
}

static UiRect ui_push_clip_box (UiBox *box) {
    box->flags |= UI_BOX_CLIPPING;
    UiRect rect = box->rect;
    rect.x += box->style.border_widths.z;
    rect.y += box->style.border_widths.y;
    rect.w -= box->style.border_widths.x + box->style.border_widths.z;
    rect.h -= box->style.border_widths.w + box->style.border_widths.y;
    return ui_push_clip_rect(rect);
}

static UiRect ui_push_clip () {
    return ui_push_clip_box(array_get_last(&ui->box_stack));
}

static UiRect ui_pop_clip () {
    array_pop(&ui->clip_stack);
    return array_get_last(&ui->clip_stack);
}

static Void animate_f32 (F32 *current, F32 final, F32 duration) {
    F32 epsilon = 0.001;
    if (fabs(*current - final) <= epsilon) {
        *current = final;
    } else {
        *current = *current + (final - *current) * (1 - powf(epsilon, ui->dt/duration));
    }
}

static Void animate_vec2 (Vec2 *current, Vec2 final, F32 duration) {
    animate_f32(&current->x, final.x, duration);
    animate_f32(&current->y, final.y, duration);
}

static Void animate_vec4 (Vec4 *current, Vec4 final, F32 duration) {
    animate_f32(&current->x, final.x, duration);
    animate_f32(&current->y, final.y, duration);
    animate_f32(&current->z, final.z, duration);
    animate_f32(&current->w, final.w, duration);
}

static Void animate_size (UiSize *current, UiSize final, F32 duration) {
    current->tag = final.tag;
    current->strictness = final.strictness;
    animate_f32(&current->value, final.value, duration);
}

static Void animate_style (UiBox *box) {
    UiStyle *a = &box->style;
    UiStyle *b = &box->next_style;
    F32 duration = box->next_style.animation_time;
    UiStyleMask mask = box->next_style.animation_mask;

    #define X(T, M, F) if (mask & M) animate_##T(&a->F, b->F, duration); else a->F = b->F;

    X(size, UI_MASK_WIDTH, size.width);
    X(size, UI_MASK_HEIGHT, size.height);
    X(vec4, UI_MASK_BG_COLOR, bg_color);
    X(vec4, UI_MASK_BG_COLOR2, bg_color2);
    X(vec4, UI_MASK_TEXT_COLOR, text_color);
    X(vec4, UI_MASK_RADIUS, radius);
    X(vec2, UI_MASK_PADDING, padding);
    X(f32, UI_MASK_SPACING, spacing);
    X(vec4, UI_MASK_BORDER_COLOR, border_color);
    X(vec4, UI_MASK_BORDER_WIDTHS, border_widths);
    X(vec4, UI_MASK_INSET_SHADOW_COLOR, inset_shadow_color);
    X(vec4, UI_MASK_OUTSET_SHADOW_COLOR, outset_shadow_color);
    X(f32, UI_MASK_INSET_SHADOW_WIDTH, inset_shadow_width);
    X(f32, UI_MASK_OUTSET_SHADOW_WIDTH, outset_shadow_width);
    X(vec2, UI_MASK_SHADOW_OFFSETS, shadow_offsets);
    X(f32, UI_MASK_BLUR_RADIUS, blur_radius);

    #undef X

    a->axis = b->axis;
    a->align[0] = b->align[0];
    a->align[1] = b->align[1];
    a->edge_softness = b->edge_softness;
    a->floating[0] = b->floating[0];
    a->floating[1] = b->floating[1];
    a->overflow[0] = b->overflow[0];
    a->overflow[1] = b->overflow[1];
}

// =============================================================================
// Style rules:
// =============================================================================
static UiPattern *pattern_alloc (Mem *mem, UiPatternTag tag) {
    Auto p = mem_new(mem, UiPattern);
    p->tag = tag;
    array_init(&p->patterns, mem);
    return p;
}

#define pattern_advance(S, N) (S)->data += N; (S)->count -= N;

static String parse_pattern_name (String *chunk) {
    U64 n = chunk->count;
    array_iter (c, chunk) {
        if (c == '#' || c == '.' || c == ':' || c == ' ') {
            n = ARRAY_IDX;
            break;
        }
    }

    if (n == 0) error_fmt("Expected selector name: [%.*s]", STR(*chunk));

    String slice = str_slice(*chunk, 0, n);
    pattern_advance(chunk, n);
    return slice;
}

static UiPattern *parse_pattern_and (String chunk, Mem *mem) {
    Auto result = pattern_alloc(mem, UI_PATTERN_AND);

    while (chunk.count) {
        Auto selector = pattern_alloc(mem, 0);
        array_push(&result->patterns, selector);

        Auto c = array_get(&chunk, 0);
        pattern_advance(&chunk, 1);

        switch (c) {
        case '*': selector->tag = UI_PATTERN_ANY; break;
        case '#': result->specificity.id++; selector->tag = UI_PATTERN_ID;  selector->string = parse_pattern_name(&chunk); break;
        case '.': result->specificity.tag++; selector->tag = UI_PATTERN_TAG; selector->string = parse_pattern_name(&chunk); break;
        case ':': {
            result->specificity.tag++;
            if      (str_starts_with(chunk, str("first"))) { pattern_advance(&chunk, 5); selector->tag = UI_PATTERN_IS_FIRST; }
            else if (str_starts_with(chunk, str("last")))  { pattern_advance(&chunk, 4); selector->tag = UI_PATTERN_IS_LAST; }
            else if (str_starts_with(chunk, str("odd")))   { pattern_advance(&chunk, 3); selector->tag = UI_PATTERN_IS_ODD; }
            else if (str_starts_with(chunk, str("even")))  { pattern_advance(&chunk, 4); selector->tag = UI_PATTERN_IS_EVEN; }
            else                                           error_fmt("Invalid pseudo tag: [%.*s]", STR(chunk));
        } break;

        default: error_fmt("Invalid selector: [%.*s]", STR(chunk));
        }
    }

    return result;
}

// Root node is UI_PATTERN_PATH and it's children UI_PATTERN_AND.
static UiPattern *parse_pattern (String pattern, Mem *mem) {
    tmem_new(tm);

    ArrayString chunks; array_init(&chunks, tm);
    str_split(pattern, str(" "), false, false, &chunks);

    Auto p = pattern_alloc(mem, UI_PATTERN_PATH);

    array_iter (chunk, &chunks) {
        UiPattern *child = parse_pattern_and(chunk, mem);
        array_push(&p->patterns, child);
        p->specificity.id += child->specificity.id;
        p->specificity.tag += child->specificity.tag;
    }

    return p;
}

static Void print_pattern (String text, UiPattern *pattern) {
    printf("%.*s\nspecificity=[%i, %i]\n\n", STR(text), pattern->specificity.id, pattern->specificity.tag);

    array_iter (chunk, &pattern->patterns) {
        printf("  %*s", cast(int, ARRAY_IDX), "");

        array_iter (selector, &chunk->patterns) {
            printf("[");
            switch (selector->tag) {
            case UI_PATTERN_ANY:      printf("*"); break;
            case UI_PATTERN_ID:       printf("#%.*s", STR(selector->string)); break;
            case UI_PATTERN_TAG:      printf(".%.*s", STR(selector->string)); break;
            case UI_PATTERN_IS_ODD:   printf(":odd"); break;
            case UI_PATTERN_IS_EVEN:  printf(":even"); break;
            case UI_PATTERN_IS_FIRST: printf(":first"); break;
            case UI_PATTERN_IS_LAST:  printf(":last"); break;
            case UI_PATTERN_PATH:     badpath;
            case UI_PATTERN_AND:      badpath;
            }
            printf("] ");
        }

        printf("\n");
    }

    printf("\n");
}

static UiStyleMask style_attr_to_mask (UiStyleAttribute attr) {
    return 1 << attr;
}

static Void ui_style_box_u32 (UiBox *box, UiStyleAttribute attr, U32 val) {
    Auto s = ui->current_style_rule ? ui->current_style_rule->style : &box->next_style;
    switch (attr) {
    case UI_ANIMATION:  s->animation_mask = val; break;
    case UI_ALIGN_X:    s->align[0] = val; break;
    case UI_ALIGN_Y:    s->align[1] = val; break;
    case UI_OVERFLOW_X: s->overflow[0] = val; break;
    case UI_OVERFLOW_Y: s->overflow[1] = val; break;
    case UI_AXIS:       s->axis = val; break;
    default:            error_fmt("Given attribute is not of type U32.");
    }
    if (ui->current_style_rule) ui->current_style_rule->mask |= style_attr_to_mask(attr);
}

static Void ui_style_box_f32 (UiBox *box, UiStyleAttribute attr, F32 val) {
    Auto s = ui->current_style_rule ? ui->current_style_rule->style : &box->next_style;
    switch (attr) {
    case UI_ANIMATION_TIME:      s->animation_time = val; break;
    case UI_BLUR_RADIUS:         s->blur_radius = val; break;
    case UI_FLOAT_X:             s->floating[0] = val; break;
    case UI_FLOAT_Y:             s->floating[1] = val; break;
    case UI_SPACING:             s->spacing = val; break;
    case UI_EDGE_SOFTNESS:       s->edge_softness = val; break;
    case UI_INSET_SHADOW_WIDTH:  s->inset_shadow_width = val; break;
    case UI_OUTSET_SHADOW_WIDTH: s->outset_shadow_width = val; break;
    default:                     error_fmt("Given attribute is not of type F32.");
    }
    if (ui->current_style_rule) ui->current_style_rule->mask |= style_attr_to_mask(attr);
}

static Void ui_style_box_vec2 (UiBox *box, UiStyleAttribute attr, Vec2 val) {
    Auto s = ui->current_style_rule ? ui->current_style_rule->style : &box->next_style;
    switch (attr) {
    case UI_PADDING:        s->padding = val; break;
    case UI_SHADOW_OFFSETS: s->shadow_offsets = val; break;
    default:                error_fmt("Given attribute is not of type Vec2.");
    }
    if (ui->current_style_rule) ui->current_style_rule->mask |= style_attr_to_mask(attr);
}

static Void ui_style_box_vec4 (UiBox *box, UiStyleAttribute attr, Vec4 val) {
    Auto s = ui->current_style_rule ? ui->current_style_rule->style : &box->next_style;
    switch (attr) {
    case UI_BG_COLOR:            s->bg_color = val; break;
    case UI_BG_COLOR2:           s->bg_color2 = val; break;
    case UI_TEXT_COLOR:          s->text_color = val; break;
    case UI_RADIUS:              s->radius = val; break;
    case UI_BORDER_COLOR:        s->border_color = val; break;
    case UI_BORDER_WIDTHS:       s->border_widths = val; break;
    case UI_INSET_SHADOW_COLOR:  s->inset_shadow_color = val; break;
    case UI_OUTSET_SHADOW_COLOR: s->outset_shadow_color = val; break;
    default:                     error_fmt("Given attribute is not of type Vec4.");
    }
    if (ui->current_style_rule) ui->current_style_rule->mask |= style_attr_to_mask(attr);
}

static Void ui_style_box_size (UiBox *box, UiStyleAttribute attr, UiSize val) {
    Auto s = ui->current_style_rule ? ui->current_style_rule->style : &box->next_style;
    switch (attr) {
    case UI_WIDTH:  s->size.width = val; break;
    case UI_HEIGHT: s->size.height = val; break;
    default:        error_fmt("Given attribute is not of type UiSize.");
    }
    if (ui->current_style_rule) ui->current_style_rule->mask |= style_attr_to_mask(attr);
}

static Void ui_style_u32  (UiStyleAttribute attr, U32 val)    { ui_style_box_u32 (array_get_last(&ui->box_stack), attr, val); }
static Void ui_style_f32  (UiStyleAttribute attr, F32 val)    { ui_style_box_f32 (array_get_last(&ui->box_stack), attr, val); }
static Void ui_style_vec2 (UiStyleAttribute attr, Vec2 val)   { ui_style_box_vec2(array_get_last(&ui->box_stack), attr, val); }
static Void ui_style_vec4 (UiStyleAttribute attr, Vec4 val)   { ui_style_box_vec4(array_get_last(&ui->box_stack), attr, val); }
static Void ui_style_size (UiStyleAttribute attr, UiSize val) { ui_style_box_size (array_get_last(&ui->box_stack), attr, val); }

static Void ui_style_rule_push (UiBox *box, String pattern) {
    if (ui->current_style_rule) error_fmt("Style rule declarations cannot be nested.");
    UiStyleRule rule = {};
    rule.pattern = parse_pattern(pattern, ui->frame_mem);
    rule.style = mem_new(ui->frame_mem, UiStyle);
    *rule.style = default_box_style;
    array_push(&box->style_rules, rule);
    ui->current_style_rule = array_ref_last(&box->style_rules);
}

static Void ui_style_rule_pop (Void *) {
    ui->current_style_rule = 0;
}

#define ui_style_rule_box(BOX, ...)\
    ui_style_rule_push(BOX, str(__VA_ARGS__));\
    if (cleanup(ui_style_rule_pop) U8 _; 1)

#define ui_style_rule(...)\
    ui_style_rule_box(array_get_last(&ui->box_stack), __VA_ARGS__)

static Bool rule_applies (UiStyleRule *rule, UiSpecificity a, UiSpecificity *specs, UiStyleAttribute attr) {
    if (! (rule->mask & style_attr_to_mask(attr))) return false;
    UiSpecificity b = specs[attr];
    return (a.id > b.id) || ((a.id == b.id) && (a.tag >= b.tag));
}

static Void apply_style_rule (UiBox *box, UiStyleRule *rule, UiSpecificity *specs) {
    Auto s = rule->pattern->specificity;
    if (rule_applies(rule, s, specs, UI_WIDTH))               { box->next_style.size.width = rule->style->size.width; specs[UI_WIDTH] = s; }
    if (rule_applies(rule, s, specs, UI_HEIGHT))              { box->next_style.size.height = rule->style->size.height; specs[UI_HEIGHT] = s; }
    if (rule_applies(rule, s, specs, UI_AXIS))                { box->next_style.axis = rule->style->axis; specs[UI_AXIS] = s; }
    if (rule_applies(rule, s, specs, UI_BG_COLOR))            { box->next_style.bg_color = rule->style->bg_color; specs[UI_BG_COLOR] = s; }
    if (rule_applies(rule, s, specs, UI_BG_COLOR2))           { box->next_style.bg_color2 = rule->style->bg_color2; specs[UI_BG_COLOR2] = s; }
    if (rule_applies(rule, s, specs, UI_TEXT_COLOR))          { box->next_style.text_color = rule->style->text_color; specs[UI_TEXT_COLOR] = s; }
    if (rule_applies(rule, s, specs, UI_RADIUS))              { box->next_style.radius = rule->style->radius; specs[UI_RADIUS] = s; }
    if (rule_applies(rule, s, specs, UI_PADDING))             { box->next_style.padding = rule->style->padding; specs[UI_PADDING] = s; }
    if (rule_applies(rule, s, specs, UI_SPACING))             { box->next_style.spacing = rule->style->spacing; specs[UI_SPACING] = s; }
    if (rule_applies(rule, s, specs, UI_ALIGN_X))             { box->next_style.align[0] = rule->style->align[0]; specs[UI_ALIGN_X] = s; }
    if (rule_applies(rule, s, specs, UI_ALIGN_Y))             { box->next_style.align[1] = rule->style->align[1]; specs[UI_ALIGN_Y] = s; }
    if (rule_applies(rule, s, specs, UI_FLOAT_X))             { box->next_style.floating[0] = rule->style->floating[0]; specs[UI_FLOAT_X] = s; }
    if (rule_applies(rule, s, specs, UI_FLOAT_Y))             { box->next_style.floating[1] = rule->style->floating[1]; specs[UI_FLOAT_Y] = s; }
    if (rule_applies(rule, s, specs, UI_OVERFLOW_X))          { box->next_style.overflow[0] = rule->style->overflow[0]; specs[UI_OVERFLOW_X] = s; }
    if (rule_applies(rule, s, specs, UI_OVERFLOW_Y))          { box->next_style.overflow[1] = rule->style->overflow[1]; specs[UI_OVERFLOW_Y] = s; }
    if (rule_applies(rule, s, specs, UI_EDGE_SOFTNESS))       { box->next_style.edge_softness = rule->style->edge_softness; specs[UI_EDGE_SOFTNESS] = s; }
    if (rule_applies(rule, s, specs, UI_BORDER_COLOR))        { box->next_style.border_color = rule->style->border_color; specs[UI_BORDER_COLOR] = s; }
    if (rule_applies(rule, s, specs, UI_BORDER_WIDTHS))       { box->next_style.border_widths = rule->style->border_widths; specs[UI_BORDER_WIDTHS] = s; }
    if (rule_applies(rule, s, specs, UI_INSET_SHADOW_COLOR))  { box->next_style.inset_shadow_color = rule->style->inset_shadow_color; specs[UI_INSET_SHADOW_COLOR] = s; }
    if (rule_applies(rule, s, specs, UI_OUTSET_SHADOW_COLOR)) { box->next_style.outset_shadow_color = rule->style->outset_shadow_color; specs[UI_OUTSET_SHADOW_COLOR] = s; }
    if (rule_applies(rule, s, specs, UI_INSET_SHADOW_WIDTH))  { box->next_style.inset_shadow_width = rule->style->inset_shadow_width; specs[UI_INSET_SHADOW_WIDTH] = s; }
    if (rule_applies(rule, s, specs, UI_OUTSET_SHADOW_WIDTH)) { box->next_style.outset_shadow_width = rule->style->outset_shadow_width; specs[UI_OUTSET_SHADOW_WIDTH] = s; }
    if (rule_applies(rule, s, specs, UI_SHADOW_OFFSETS))      { box->next_style.shadow_offsets = rule->style->shadow_offsets; specs[UI_SHADOW_OFFSETS] = s; }
    if (rule_applies(rule, s, specs, UI_BLUR_RADIUS))         { box->next_style.blur_radius = rule->style->blur_radius; specs[UI_BLUR_RADIUS] = s; }
    if (rule_applies(rule, s, specs, UI_ANIMATION))           { box->next_style.animation_mask = rule->style->animation_mask; specs[UI_ANIMATION] = s; }
    if (rule_applies(rule, s, specs, UI_ANIMATION_TIME))      { box->next_style.animation_time = rule->style->animation_time; specs[UI_ANIMATION_TIME] = s; }
}

static Bool match_pattern (UiBox *box, UiPattern *pattern) {
    assert_dbg(pattern->tag == UI_PATTERN_AND);

    U64 box_idx = box->parent ? array_find(&box->parent->children, IT == box) : 0;
    assert_dbg(box_idx != ARRAY_NIL_IDX);

    Bool result = true;
    array_iter (selector, &pattern->patterns) {
        switch (selector->tag) {
        case UI_PATTERN_ID:       result = str_match(box->label, selector->string); break;
        case UI_PATTERN_IS_ODD:   result = (box_idx % 2); break;
        case UI_PATTERN_IS_EVEN:  result = !(box_idx % 2); break;
        case UI_PATTERN_IS_FIRST: result = (box_idx == 0); break;
        case UI_PATTERN_IS_LAST:  result = (box_idx == box->parent->children.count - 1); break;
        case UI_PATTERN_TAG:      result = array_find_ref(&box->tags, str_match(*IT, selector->string)); break;
        case UI_PATTERN_ANY:      break;
        case UI_PATTERN_PATH:     badpath;
        case UI_PATTERN_AND:      badpath;
        }

        if (! result) break;
    }

    return result;
}

static UiStyleRule derive_new_rule (UiStyleRule *old_rule, Mem *mem) {
    UiStyleRule new_rule = {};
    new_rule.style = old_rule->style;
    new_rule.mask = old_rule->mask;
    new_rule.pattern = pattern_alloc(mem, UI_PATTERN_PATH);
    *new_rule.pattern = *old_rule->pattern;
    new_rule.pattern->patterns.data++;
    new_rule.pattern->patterns.count--;
    return new_rule;
}

static Void apply_style_rules_box (UiBox *box, ArrayUiStyleRule *active_rules, Mem *mem) {
    U64 restore_point = active_rules->count;
    array_push_many(active_rules, &box->style_rules);

    UiSpecificity specs[UI_ATTRIBUTE_COUNT] = {};

    Auto stop_at = active_rules->count - 1; // Don't loop over newly added derived rules.
    array_iter (rule, active_rules, *) {
        UiPattern *head_of_rule = array_get(&rule->pattern->patterns, 0);
        Bool match = match_pattern(box, head_of_rule);

        if (match) {
            if (rule->pattern->patterns.count == 1) {
                apply_style_rule(box, rule, specs);
            } else {
                array_push(active_rules, derive_new_rule(rule, mem));
            }
        }

        if (ARRAY_IDX == stop_at) break;
    }

    array_iter (child, &box->children) apply_style_rules_box(child, active_rules, mem);
    active_rules->count = restore_point;
    animate_style(box);
}

static Void apply_style_rules () {
    tmem_new(tm);
    ArrayUiStyleRule active_rules;
    array_init(&active_rules, tm);
    apply_style_rules_box(ui->root, &active_rules, tm);
}

static Void ui_tag_box_str (UiBox *box, String tag)  { array_push(&box->tags, tag); }
static Void ui_tag_str     (String tag)              { return ui_tag_box_str(array_get_last(&ui->box_stack), tag); }
static Void ui_tag_box     (UiBox *box, CString tag) { return ui_tag_box_str(box, str(tag)); }
static Void ui_tag         (CString tag)             { return ui_tag_box_str(array_get_last(&ui->box_stack), str(tag)); }

// =============================================================================
// Layout:
// =============================================================================
static Void compute_standalone_sizes (U64 axis) {
    array_iter (box, &ui->depth_first) {
        Auto size = &box->style.size.v[axis];

        if (size->tag == UI_SIZE_PIXELS) {
            box->rect.size[axis] = size->value;
        } else if (size->tag == UI_SIZE_TEXT) {
            box->rect.size[axis] = box->text_rect.size[axis] + 2*box->style.padding.v[axis];
        }
    }
}

static Void compute_downward_dependent_sizes (U64 axis) {
    array_iter_back (box, &ui->depth_first) {
        Auto size = &box->style.size.v[axis];
        if (size->tag != UI_SIZE_CHILDREN_SUM) continue;

        array_iter (child, &box->children) {
            if (child->style.size.v[axis].tag == UI_SIZE_PCT_PARENT) {
                // Cycle: parent defined by child and child defined by parent.
                size->tag = UI_SIZE_PCT_PARENT;
                size->value = 1;
                continue;
            }
        }

        F32 final_size = 2*box->style.padding.v[axis];
        if (box->style.axis == axis) {
            array_iter (child, &box->children) {
                if (! isnan(child->style.floating[axis])) continue;
                final_size += child->rect.size[axis];
                if (! ARRAY_ITER_DONE) final_size += box->style.spacing;
            }
        } else {
            array_iter (child, &box->children) {
                if (! isnan(child->style.floating[axis])) continue;
                final_size = max(final_size, child->rect.size[axis] + 2*box->style.padding.v[axis]);
            }
        }

        box->rect.size[axis] = final_size;
    }
}

static Void compute_upward_dependent_sizes (U64 axis) {
    array_iter (box, &ui->depth_first) {
        Auto size = &box->style.size.v[axis];
        if (size->tag == UI_SIZE_PCT_PARENT) box->rect.size[axis] = size->value * (box->parent->rect.size[axis] - 2*box->parent->style.padding.v[axis]);
    }
}

static Void fix_overflow (U64 axis) {
    array_iter (box, &ui->depth_first) {
        F32 box_size = box->rect.size[axis] - 2*box->style.padding.v[axis];

        if (box->style.axis == axis) {
            F32 children_size = 0;
            array_iter (child, &box->children) {
                if (! isnan(child->style.floating[axis])) continue;
                children_size += child->rect.size[axis];
                if (! ARRAY_ITER_DONE) children_size += box->style.spacing;
            }

            if (box_size < children_size && !box->style.overflow[axis]) {
                F32 overflow = children_size - box_size;

                F32 total_slack = 0;
                array_iter (child, &box->children) {
                    if (! isnan(child->style.floating[axis])) continue;
                    total_slack += child->rect.size[axis] * (1 - child->style.size.v[axis].strictness);
                }

                if (total_slack >= overflow) {
                    F32 slack_fraction = overflow / total_slack;
                    array_iter (child, &box->children) {
                        if (! isnan(child->style.floating[axis])) continue;
                        child->rect.size[axis] -= child->rect.size[axis] * (1 - child->style.size.v[axis].strictness) * slack_fraction;
                    }
                }
            }
        } else {
            array_iter (child, &box->children) {
                if (! isnan(child->style.floating[axis])) continue;
                if (box_size >= child->rect.size[axis]) continue;
                if (box->style.overflow[axis]) continue;
                F32 overflow = child->rect.size[axis] - box_size;
                F32 slack = child->rect.size[axis] * (1 - child->style.size.v[axis].strictness);
                if (slack >= overflow) child->rect.size[axis] -= overflow;
            }
        }
    }
}

static Void compute_positions (U64 axis) {
    array_iter (box, &ui->depth_first) {
        if (box->style.axis == axis) {
            F32 content_size = 2*box->style.padding.v[axis];
            array_iter (child, &box->children) {
                if (! isnan(child->style.floating[axis])) continue;
                content_size += child->rect.size[axis];
                if (! ARRAY_ITER_DONE) content_size += box->style.spacing;
            }

            box->content.size[axis] = floor(content_size);

            F32 align_offset = 0;
            switch (box->style.align[axis]) {
            case UI_ALIGN_START:  break;
            case UI_ALIGN_MIDDLE: align_offset = floor(box->rect.size[axis]/2 - content_size/2); break;
            case UI_ALIGN_END:    align_offset = box->rect.size[axis] - content_size; break;
            }

            F32 pos = box->rect.top_left.v[axis] + box->style.padding.v[axis] + align_offset + box->content.top_left.v[axis];
            array_iter (child, &box->children) {
                if (isnan(child->style.floating[axis])) {
                    child->rect.top_left.v[axis] = pos;
                    pos += child->rect.size[axis] + box->style.spacing;
                } else {
                    child->rect.top_left.v[axis] = box->rect.top_left.v[axis] + child->style.floating[axis];
                }
            }
        } else {
            box->content.size[axis] = 0;

            array_iter (child, &box->children) {
                if (isnan(child->style.floating[axis])) {
                    F32 content_size = child->rect.size[axis] + 2*box->style.padding.v[axis];

                    box->content.size[axis] = floor(max(box->content.size[axis], content_size));

                    F32 align_offset = 0;
                    switch (box->style.align[axis]) {
                    case UI_ALIGN_START:  break;
                    case UI_ALIGN_MIDDLE: align_offset = floor(box->rect.size[axis]/2 - content_size/2); break;
                    case UI_ALIGN_END:    align_offset = box->rect.size[axis] - content_size; break;
                    }

                    child->rect.top_left.v[axis] = box->rect.top_left.v[axis] + box->style.padding.v[axis] + align_offset + box->content.top_left.v[axis];
                } else {
                    child->rect.top_left.v[axis] = box->rect.top_left.v[axis] + child->style.floating[axis];
                }
            }
        }

        array_iter (child, &box->children) {
            child->rect.x = floor(child->rect.x);
            child->rect.y = floor(child->rect.y);
            child->rect.w = floor(child->rect.w);
            child->rect.h = floor(child->rect.h);
        }
    }
}

static Void compute_layout () {
    for (U64 axis = 0; axis < 2; ++axis) {
        compute_standalone_sizes(axis);
        compute_downward_dependent_sizes(axis);
        compute_upward_dependent_sizes(axis);
        fix_overflow(axis);
        compute_positions(axis);
    }
}

static Void find_topmost_hovered_box (UiBox *box) {
    if (! (box->flags & UI_BOX_CLICK_THROUGH)) {
        UiRect r = compute_rect_intersect(box->rect, array_get_last(&ui->clip_stack));
        if (within_box(r, ui->mouse)) ui->hovered = box;
    }

    if (box->flags & UI_BOX_CLIPPING) ui_push_clip_box(box);
    array_iter (child, &box->children) find_topmost_hovered_box(child);
    if (box->flags & UI_BOX_CLIPPING) ui_pop_clip();
}

static Void render_text (String text, Vec4 color, F32 x, F32 y, UiRect *out_rect) {
    tmem_new(tm);

    glBindTexture(GL_TEXTURE_2D, ui->glyph_cache->atlas_texture);

    ArrayString lines;
    array_init(&lines, tm);
    str_split(text, str("\n"), 0, 0, &lines);

    U32 line_height = ui->glyph_cache->font_size;
    U32 line_spacing = 2;
    U32 widest_line = 0;
    U32 y_offset = 0;

    array_iter (line, &lines) {
        tmem_new(tm);
        SliceGlyphInfo infos = get_glyph_infos(ui->glyph_cache, tm, line);

        array_iter (info, &infos, *) {
            GlyphSlot *slot = glyph_cache_get(ui->glyph_cache, info);

            Vec2 top_left = {x + info->x + slot->bearing_x, y + y_offset + info->y - slot->bearing_y};
            Vec2 bottom_right = {top_left.x + slot->width, top_left.y + slot->height};

            draw_rect(
                .top_left     = top_left,
                .bottom_right = bottom_right,
                .texture_rect = {slot->x, slot->y, slot->width, slot->height},
                .text_color   = color,
                .text_is_grayscale = (slot->pixel_mode == FT_PIXEL_MODE_GRAY),
            );

            if (ARRAY_ITER_DONE) {
                U32 line_width = info->x + slot->bearing_x + info->x_advance;
                widest_line = max(widest_line, line_width);
            }
        }

        y_offset += line_height + line_spacing;
    }

    out_rect->x = x;
    out_rect->y = y;
    out_rect->w = widest_line;
    out_rect->h = y_offset - line_spacing;
}

static Void render_box (UiBox *box) {
    if (box->style.blur_radius) {
        flush_vertices();

        F32 blur_radius = max(1, cast(Int, box->style.blur_radius));

        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blur_buffer1);
        glBlitFramebuffer(0, 0, win_width, win_height, 0, 0, win_width/BLUR_SHRINK, win_height/BLUR_SHRINK, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glViewport(0, 0, win_width/BLUR_SHRINK, win_height/BLUR_SHRINK);

        glBindVertexArray(blur_VAO);

        blur_vertices.count = 0;
        array_push_lit(&blur_vertices, -1.0f,  1.0f);
        array_push_lit(&blur_vertices, -1.0f, -1.0f);
        array_push_lit(&blur_vertices,  1.0f, -1.0f);
        array_push_lit(&blur_vertices, -1.0f,  1.0f);
        array_push_lit(&blur_vertices,  1.0f, -1.0f);
        array_push_lit(&blur_vertices,  1.0f,  1.0f);

        glBindBuffer(GL_ARRAY_BUFFER, blur_VBO);
        ATTR(AElem(&blur_vertices), 0, 2, pos);
        glBufferData(GL_ARRAY_BUFFER, array_size(&blur_vertices), blur_vertices.data, GL_STREAM_DRAW);

        glUseProgram(blur_shader);
        set_int(blur_shader, "blur_radius", blur_radius);
        set_bool(blur_shader, "do_blurring", true);
        set_mat4(blur_shader, "projection", mat4(1));

        for (U64 i = 0; i < 3; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, blur_buffer2);
            glBindTexture(GL_TEXTURE_2D, blur_tex1);
            set_bool(blur_shader, "horizontal", true);
            glDrawArrays(GL_TRIANGLES, 0, blur_vertices.count);

            glBindFramebuffer(GL_FRAMEBUFFER, blur_buffer1);
            glBindTexture(GL_TEXTURE_2D, blur_tex2);
            set_bool(blur_shader, "horizontal", false);
            glDrawArrays(GL_TRIANGLES, 0, blur_vertices.count);
        }

        glViewport(0, 0, win_width, win_height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

        UiRect r = box->rect;
        r.y = win_height - r.y;
        blur_vertices.count = 0;
        array_push_lit(&blur_vertices, r.x, r.y);
        array_push_lit(&blur_vertices, r.x+r.w, r.y);
        array_push_lit(&blur_vertices, r.x, r.y-r.h);
        array_push_lit(&blur_vertices, r.x, r.y-r.h);
        array_push_lit(&blur_vertices, r.x+r.w, r.y);
        array_push_lit(&blur_vertices, r.x+r.w, r.y-r.h);

        set_mat4(blur_shader, "projection", projection);
        set_bool(blur_shader, "do_blurring", false);
        set_vec2(blur_shader, "half_size", vec2(r.w/2, r.h/2));
        set_vec2(blur_shader, "center", vec2(r.x+r.w/2, r.y-r.h/2));
        set_vec4(blur_shader, "radius", box->style.radius);
        set_float(blur_shader, "blur_shrink", BLUR_SHRINK);

        ATTR(AElem(&blur_vertices), 0, 2, pos);
        glBufferData(GL_ARRAY_BUFFER, array_size(&blur_vertices), blur_vertices.data, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, blur_vertices.count);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    if (! (box->flags & UI_BOX_INVISIBLE)) draw_rect(
        .top_left            = box->rect.top_left,
        .bottom_right        = vec2(box->rect.x + box->rect.w, box->rect.y + box->rect.h),
        .color               = box->style.bg_color,
        .color2              = box->style.bg_color2,
        .radius              = box->style.radius,
        .edge_softness       = box->style.edge_softness,
        .border_color        = box->style.border_color,
        .border_widths       = box->style.border_widths,
        .inset_shadow_color  = box->style.inset_shadow_color,
        .outset_shadow_color = box->style.outset_shadow_color,
        .inset_shadow_width  = box->style.inset_shadow_width,
        .outset_shadow_width = box->style.outset_shadow_width,
        .shadow_offsets      = box->style.shadow_offsets,
    );

    if (box->flags & UI_BOX_CLIPPING) {
        flush_vertices();
        UiRect r = ui_push_clip_box(box);
        glScissor(r.x, win_height - r.y - r.h, r.w, r.h);
    }

    array_iter (c, &box->children) render_box(c);

    if (box->flags & UI_BOX_DRAW_TEXT) {
        F32 x = floor(box->rect.x + box->rect.w/2 - box->text_rect.w/2);
        F32 y = floor(box->rect.y + box->rect.h/2 + box->text_rect.h/2);
        render_text(box->label, box->style.text_color, x, y, &box->text_rect);
    }

    if (box->flags & UI_BOX_CLIPPING) {
        flush_vertices();
        UiRect r = ui_pop_clip();
        glScissor(r.x, win_height - r.y - r.h, r.w, r.h);
    }
}

// =============================================================================
// Widgets:
// =============================================================================
static UiBox *ui_hspacer () {
    UiBox *box = ui_box(UI_BOX_INVISIBLE, "hspacer") { ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0}); }
    return box;
}

static UiBox *ui_vspacer () {
    UiBox *box = ui_box(UI_BOX_INVISIBLE, "vspacer") { ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0}); }
    return box;
}

static UiBox *ui_button_str (String label) {
    UiBox *button = ui_box_str(UI_BOX_REACTIVE|UI_BOX_CAN_FOCUS|UI_BOX_DRAW_TEXT, label) {
        ui_tag("button");

        if (button->signal.hovered) {
            ui_push_clip_box(button);
            ui_box(UI_BOX_CLICK_THROUGH, "button_highlight") {
                F32 s = button->rect.h/8;
                ui_style_f32(UI_EDGE_SOFTNESS, 60);
                ui_style_vec4(UI_RADIUS, vec4(s, s, s, s));
                ui_style_f32(UI_FLOAT_X, ui->mouse.x - button->rect.x - s);
                ui_style_f32(UI_FLOAT_Y, ui->mouse.y - button->rect.y - s);
                ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .2));
                ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, 2*s, 1});
                ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 2*s, 1});
            }
            ui_pop_clip();
        }
    }

    return button;
}

static UiBox *ui_button_fmt (CString label, ...) {
    tmem_new(tm);
    AString a = astr_new(tm);
    astr_push_fmt_vam(&a, label);
    return ui_button_str(astr_to_str(&a));
}

static UiBox *ui_button (CString label) {
    return ui_button_str(str(label));
}

static UiBox *ui_vscroll_bar (String label, UiRect rect, F32 ratio, F32 *val) {
    UiBox *container = ui_box_str(UI_BOX_REACTIVE, label) {
        F32 padding = container->style.padding.x;

        ui_style_f32(UI_FLOAT_X, rect.x - 2*padding);
        ui_style_f32(UI_FLOAT_Y, rect.y);
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_CHILDREN_SUM, 0, 1});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, rect.h, 0});
        ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .4));
        ui_style_u32(UI_AXIS, UI_AXIS_VERTICAL);
        ui_style_vec2(UI_PADDING, vec2(4, 4));
        ui_style_f32(UI_EDGE_SOFTNESS, 0);

        if (container->signal.pressed) {
            *val = ui->mouse.y - container->rect.y - ratio*rect.h/2;
            *val = clamp(*val, 0, (1-ratio)*rect.h);
        }

        if (container->signal.hovered && (ui->event->tag == EVENT_SCROLL)) {
            *val -= (15 * ui->event->y);
            *val = clamp(*val, 0, (1-ratio)*rect.h);
            ui->event->tag = EVENT_EATEN;
        }

        ui_style_rule(".hover #scroll_bar_knob") { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule(".press #scroll_bar_knob") { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule("#scroll_bar_knob.hover")  { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule("#scroll_bar_knob.press")  { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }

        ui_box(UI_BOX_CLICK_THROUGH|UI_BOX_INVISIBLE, "scroll_bar_spacer") {
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, *val, 0});
        }

        UiBox *knob = ui_box(UI_BOX_REACTIVE, "scroll_bar_knob") {
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, rect.w, 1});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, ratio*rect.h, 1});
            ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .4));
            ui_style_f32(UI_EDGE_SOFTNESS, 0);

            if (knob->signal.pressed && ui->event->tag == EVENT_MOUSE_MOVE) {
                *val += ui->mouse_dt.y;
                *val = clamp(*val, 0, (1-ratio)*rect.h);
            }
        }
    }

    return container;
}

static UiBox *ui_hscroll_bar (String label, UiRect rect, F32 ratio, F32 *val) {
    UiBox *container = ui_box_str(UI_BOX_REACTIVE, label) {
        F32 padding = container->style.padding.y;

        ui_style_f32(UI_FLOAT_X, rect.x);
        ui_style_f32(UI_FLOAT_Y, rect.y - 2*padding);
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, rect.w, 1});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_CHILDREN_SUM, 0, 1});
        ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .4));
        ui_style_u32(UI_AXIS, UI_AXIS_HORIZONTAL);
        ui_style_vec2(UI_PADDING, vec2(4, 4));
        ui_style_f32(UI_EDGE_SOFTNESS, 0);

        if (container->signal.pressed) {
            *val = ui->mouse.x - container->rect.x - ratio*rect.w/2;
            *val = clamp(*val, 0, (1-ratio)*rect.w);
        }

        if (container->signal.hovered && (ui->event->tag == EVENT_SCROLL)) {
            *val -= (25 * ui->event->y);
            *val = clamp(*val, 0, (1-ratio)*rect.w);
            ui->event->tag = EVENT_EATEN;
        }

        ui_style_rule(".hover #scroll_bar_knob") { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule(".press #scroll_bar_knob") { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule("#scroll_bar_knob.hover")  { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }
        ui_style_rule("#scroll_bar_knob.press")  { ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8)); }

        ui_box(UI_BOX_CLICK_THROUGH|UI_BOX_INVISIBLE, "scroll_bar_spacer") {
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, *val, 0});
        }

        UiBox *knob = ui_box(UI_BOX_REACTIVE, "scroll_bar_knob") {
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, rect.h, 1});
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, ratio*rect.w, 1});
            ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .4));
            ui_style_f32(UI_EDGE_SOFTNESS, 0);

            if (knob->signal.pressed && ui->event->tag == EVENT_MOUSE_MOVE) {
                *val += ui->mouse_dt.x;
                *val = clamp(*val, 0, (1-ratio)*rect.w);
            }
        }
    }

    return container;
}

static UiBox *ui_scroll_box_push (String label) {
    UiBox *container = ui_box_push_str(UI_BOX_REACTIVE, label);
    ui_style_box_u32(container, UI_OVERFLOW_X, true);
    ui_style_box_u32(container, UI_OVERFLOW_Y, true);
    container->scratch = ui->depth_first.count-1;
    ui_push_clip_box(container);
    return container;
}

static Void ui_scroll_box_pop () {
    UiBox *container = array_get_last(&ui->box_stack);

    Bool contains_focused = (ui->focus_idx >= container->scratch);
    if (contains_focused && ui->event->tag == EVENT_KEY_PRESS && ui->event->key == GLFW_KEY_TAB) {
        F32 fx1 = ui->focused->rect.x + ui->focused->rect.w;
        F32 cx1 = container->rect.x + container->rect.w;
        if (fx1 > cx1) {
            container->content.x -= fx1 - cx1;
        } else if (ui->focused->rect.x < container->rect.x) {
            container->content.x += container->rect.x - ui->focused->rect.x;
        }

        F32 fy1 = ui->focused->rect.y + ui->focused->rect.h;
        F32 cy1 = container->rect.y + container->rect.h;
        if (fy1 > cy1) {
            container->content.y -= fy1 - cy1;
        } else if (ui->focused->rect.y < container->rect.y) {
            container->content.y += container->rect.y - ui->focused->rect.y;
        }
    }

    F32 speed = 25;
    F32 bar_width = 8;

    if (container->rect.w < container->content.w) {
        F32 scroll_val = (fabs(container->content.x) / container->content.w) * container->rect.w;
        F32 ratio = container->rect.w / container->content.w;
        ui_hscroll_bar(str("scroll_bar_x"), (UiRect){0, container->rect.h - bar_width, container->rect.w, bar_width}, ratio, &scroll_val);
        container->content.x = -(scroll_val/container->rect.w*container->content.w);

        if (container->signal.hovered && (ui->event->tag == EVENT_SCROLL) && is_key_pressed(GLFW_KEY_LEFT_CONTROL)) {
            container->content.x += speed * ui->event->y;
            ui->event->tag = EVENT_EATEN;
        }

        container->content.x = clamp(container->content.x, -(container->content.w - container->rect.w), 0);
    } else {
        container->content.x = 0;
    }

    if (container->rect.h < container->content.h) {
        F32 scroll_val = (fabs(container->content.y) / container->content.h) * container->rect.h;
        F32 ratio = container->rect.h / container->content.h;
        ui_vscroll_bar(str("scroll_bar_y"), (UiRect){container->rect.w - bar_width, 0, bar_width, container->rect.h}, ratio, &scroll_val);
        container->content.y = -(scroll_val/container->rect.h*container->content.h);

        if (container->signal.hovered && (ui->event->tag == EVENT_SCROLL) && !is_key_pressed(GLFW_KEY_LEFT_CONTROL)) {
            container->content.y += speed * ui->event->y;
            ui->event->tag = EVENT_EATEN;
        }

        container->content.y = clamp(container->content.y, -(container->content.h - container->rect.h), 0);
    } else {
        container->content.y = 0;
    }

    ui_pop_clip();
    ui_pop_parent();
}

static Void ui_scroll_box_pop_ (Void *) {
    ui_scroll_box_pop();
}

#define ui_scroll_box(LABEL)\
    ui_scroll_box_push(str(LABEL));\
    if (cleanup(ui_scroll_box_pop_) U8 _; 1)

static UiBox *ui_slider_str (String label, F32 *val) {
    UiBox *container = ui_box_str(UI_BOX_REACTIVE|UI_BOX_CAN_FOCUS, label) {
        ui_tag("slider");
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 20, 1});
        ui_style_f32(UI_EDGE_SOFTNESS, 0);
        ui_style_f32(UI_SPACING, 0);
        ui_style_u32(UI_ALIGN_Y, UI_ALIGN_MIDDLE);

        ui_style_rule(".focus") {
            ui_style_vec4(UI_BORDER_WIDTHS, vec4(2, 2, 2, 2));
            ui_style_vec4(UI_BORDER_COLOR, vec4(1, 1, 1, .8));
        }

        if (container->signal.focused && (ui->event->tag == EVENT_KEY_PRESS) && (ui->event->key == GLFW_KEY_LEFT)) {
            *val -= .1;
            *val = clamp(*val, 0, 1);
            ui->event->tag = EVENT_EATEN;
        }

        if (container->signal.focused && (ui->event->tag == EVENT_KEY_PRESS) && (ui->event->key == GLFW_KEY_RIGHT)) {
            *val += .1;
            *val = clamp(*val, 0, 1);
            ui->event->tag = EVENT_EATEN;
        }

        if (container->signal.pressed) {
            *val = (ui->mouse.x - container->rect.x) / container->rect.w;
            *val = clamp(*val, 0, 1);
        }

        if (container->signal.hovered && (ui->event->tag == EVENT_SCROLL)) {
            *val = *val - (10*ui->event->y) / container->rect.w;
            *val = clamp(*val, 0, 1);
            ui->event->tag = EVENT_EATEN;
        }

        ui_box(UI_BOX_CLICK_THROUGH, "slider_track") {
            ui_style_f32(UI_FLOAT_X, 0);
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 4, 0});
            ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .8));
            ui_style_f32(UI_EDGE_SOFTNESS, 0);

            ui_box(0, "slider_track_fill") {
                ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, *val, 0});
                ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
                ui_style_vec4(UI_BG_COLOR, vec4(1, 0, 1, .8));
                ui_style_f32(UI_EDGE_SOFTNESS, 0);
            }
        }

        F32 knob_size = max(8, container->rect.h - 8);

        ui_box(UI_BOX_CLICK_THROUGH|UI_BOX_INVISIBLE, "slider_spacer") {
            F32 spacer_width = max(0, *val - knob_size/(2*max(knob_size, container->rect.w)));
            assert_dbg(spacer_width <= 1.0);
            assert_dbg(spacer_width >= 0.0);
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, spacer_width, 0});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 2, 0});
            ui_style_f32(UI_EDGE_SOFTNESS, 0);
        }

        ui_box(UI_BOX_CLICK_THROUGH, "slider_knob") {
            ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, 1));
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, knob_size, 1});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, knob_size, 1});
            ui_style_vec4(UI_RADIUS, vec4(knob_size/2, knob_size/2, knob_size/2, knob_size/2));
        }
    }

    return container;
}

static UiBox *ui_slider (CString label, F32 *val) {
    return ui_slider_str(str(label), val);
}

// Note that the grid will only display correctly as long as
// it's size is set to UI_SIZE_PIXELS or UI_SIZE_PCT_PARENT.
// If it has a downwards dependent size (UI_SIZE_CHILDREN_SUM)
// it will just collapse.
//
// The children of the grid must only be grid_cells:
//
//     ui_grid() {
//         ui_grid_cell(){}
//         ui_grid_cell(){}
//         ui_grid_cell(){}
//     }
//
static UiBox *ui_grid_push (String label) {
    UiBox *grid = ui_box_push_str(0, label);
    ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
    ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
    return grid;
}

static Void ui_grid_pop () {
    UiBox *grid = array_get_last(&ui->box_stack);

    F32 rows = 0;
    F32 cols = 0;

    array_iter (cell, &grid->children) {
        Vec4 coords = *cast(Vec4*, cell->scratch);
        if ((coords.x + coords.z) > rows) rows = coords.x + coords.z;
        if ((coords.y + coords.w) > cols) cols = coords.y + coords.w;
    }

    F32 cell_width  = floor(grid->rect.w / rows);
    F32 cell_height = floor(grid->rect.h / cols);

    array_iter (cell, &grid->children) {
        Vec4 coords = *cast(Vec4*, cell->scratch);
        cell->next_style.floating[0] = coords.x * cell_width;
        cell->next_style.floating[1] = coords.y * cell_height;
        cell->next_style.size.width  = (UiSize){UI_SIZE_PIXELS, coords.z * cell_width, 1};
        cell->next_style.size.height = (UiSize){UI_SIZE_PIXELS, coords.w * cell_height, 1};
    }

    ui_pop_parent();
}

static Void ui_grid_pop_ (Void *) { ui_grid_pop(); }

#define ui_grid(L)\
    ui_grid_push(str(L));\
    if (cleanup(ui_grid_pop_) U8 _; 1)

// The unit of measurement for the x/y/w/h parameters is basic cells.
// That is, think of the grid as made up of basic cells. This function
// constructs super cells by defining on which basic cell they start,
// and how many basic cells they cover.
static UiBox *ui_grid_cell_push (F32 x, F32 y, F32 w, F32 h) {
    UiBox *cell = ui_box_push_fmt(0, "grid_cell_%f_%f", x, y);
    ui_style_f32(UI_FLOAT_X, 0);
    ui_style_f32(UI_FLOAT_Y, 0);
    ui_style_vec2(UI_PADDING, vec2(8, 8));
    ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .4));
    ui_style_vec4(UI_BORDER_WIDTHS, vec4(1, 1, 1, 1));
    ui_style_vec4(UI_BORDER_COLOR, vec4(0, 0, 0, .8));
    ui_style_f32(UI_EDGE_SOFTNESS, 0);

    Vec4 *coords = mem_new(ui->frame_mem, Vec4);
    coords->x = x;
    coords->y = y;
    coords->z = w;
    coords->w = h;
    cell->scratch = cast(U64, coords);

    return cell;
}

static Void ui_grid_cell_pop  ()       { ui_pop_parent(); }
static Void ui_grid_cell_pop_ (Void *) { ui_grid_cell_pop(); }

#define ui_grid_cell(...)\
    ui_grid_cell_push(__VA_ARGS__);\
    if (cleanup(ui_grid_cell_pop_) U8 _; 1)

// =============================================================================
// Frame:
// =============================================================================
static Void build_main_view () {
    ui_scroll_box("main_view") {
        ui_tag("vbox");
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 3./4, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_u32(UI_ANIMATION, UI_MASK_WIDTH);

        ui_style_rule("#main_view") { ui_style_vec2(UI_PADDING, vec2(80, 16)); }

        ui_box(0, "box2_0") {
            ui_tag("hbox");
            ui_tag("item");

            ui_style_rule("#Foo4") { ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0}); }
            ui_button("Foo4");
            ui_button("Foo5");
        }

        ui_box(0, "box2_1") {
            ui_tag("hbox");
            ui_tag("item");

            ui_button("Foo6");
            ui_button("Foo7");
        }

        ui_box(0, "box2_2") {
            ui_tag("hbox");
            ui_tag("item");
            ui_style_u32(UI_ALIGN_X, UI_ALIGN_MIDDLE);

            ui_button("Foo8");
            ui_button("Foo9");
        }

        ui_box(0, "box2_3") {
            ui_tag("hbox");
            ui_tag("item");
            ui_style_u32(UI_ALIGN_X, UI_ALIGN_END);

            ui_button("Foo10");
            ui_button("Foo11");
        }

        static F32 n = .5;

        ui_scroll_box("box2_4") {
            ui_tag("hbox");
            ui_tag("item");
            for (U64 i = 0; i < cast(U64, 10*n); ++i) ui_button_fmt("Foo_%i", i);
        }

        for (U64 i = 0; i < 20; ++i) {
            ui_box_fmt(0, "box2__%i", i) {
                ui_tag("hbox");
                ui_tag("item");
                ui_slider("Slider", &n);
            }
        }
    }
}

static Void build_second_view () {
    ui_scroll_box("second_view") {
        ui_tag("vbox");
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 3./4, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_u32(UI_ANIMATION, UI_MASK_WIDTH);

        ui_style_rule("#second_view") { ui_style_vec2(UI_PADDING, vec2(80, 16)); }

        ui_grid("test_grid") {
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 3./4, 0});

            ui_grid_cell(0, 0, 3, 2) { ui_button("1"); }
            ui_grid_cell(3, 0, 5, 2) { ui_button("1"); }
            ui_grid_cell(0, 2, 3, 5) { ui_button("1"); }
            ui_grid_cell(3, 2, 5, 2) { ui_button("1"); }
            ui_grid_cell(3, 4, 3, 2) {
                ui_grid("test_grid") {
                    ui_grid_cell(0, 0, 3, 2);
                    ui_grid_cell(3, 0, 5, 2);
                    ui_grid_cell(0, 2, 3, 5);
                    ui_grid_cell(3, 2, 5, 2);
                    ui_grid_cell(3, 4, 3, 2);
                    ui_grid_cell(6, 4, 2, 2);
                    ui_grid_cell(3, 6, 5, 1);
                }
            }
            ui_grid_cell(6, 4, 2, 2) { ui_button("1"); }
            ui_grid_cell(3, 6, 5, 1) { ui_button("1"); }
        }
    }
}

static Bool show_modal () {
    ui_parent(ui->root) {
        UiBox *overlay = ui_box(UI_BOX_REACTIVE, "modal_bg") {
            ui_style_f32(UI_FLOAT_X, 0);
            ui_style_f32(UI_FLOAT_Y, 0);
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
            ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .2));

            if ((ui->event->tag == EVENT_KEY_PRESS) && (ui->event->key == GLFW_KEY_ESCAPE)) return false;
            if (overlay->signal.clicked) return false;

            static F32 x = 1;
            static F32 y = 1;

            UiBox *modal = ui_box(UI_BOX_REACTIVE, "modal") {
                if (modal->signal.pressed && (ui->event->tag == EVENT_MOUSE_MOVE)) {
                    x += ui->mouse_dt.x;
                    y += ui->mouse_dt.y;
                }

                ui_style_f32(UI_FLOAT_X, x);
                ui_style_f32(UI_FLOAT_Y,  y);
                ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, 400, 1});
                ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 200, 1});
                ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .6));
                ui_style_vec4(UI_RADIUS, vec4(8, 8, 8, 8));
                ui_style_vec2(UI_PADDING, vec2(8, 8));
                ui_style_vec4(UI_BORDER_COLOR, vec4(0, 0, 0, .6));
                ui_style_vec4(UI_BORDER_WIDTHS, vec4(1, 1, 1, 1));
                ui_style_f32(UI_OUTSET_SHADOW_WIDTH, 2);
                ui_style_vec4(UI_OUTSET_SHADOW_COLOR, vec4(0, 0, 0, 1));
                ui_style_f32(UI_ANIMATION_TIME, 1);
                ui_style_f32(UI_BLUR_RADIUS, 2);
                ui_style_u32(UI_ANIMATION, UI_MASK_BG_COLOR);

                ui_style_rule(".button") {
                    ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_TEXT, 0, 0});
                    ui_style_vec2(UI_PADDING, vec2(4, 4));
                }

                ui_button("modal_button");
            }
        }
    }

    return true;
}

static Void build () {
    ui_style_rule(".button") {
        ui_style_vec4(UI_BG_COLOR, hsva2rgba(vec4(.8, .4, 1, .8f)));
        ui_style_vec4(UI_BG_COLOR2, hsva2rgba(vec4(.8, .4, .6, .8f)));
        ui_style_vec4(UI_RADIUS, vec4(4, 4, 4, 4));
        ui_style_vec2(UI_SHADOW_OFFSETS, vec2(0, -2));
        ui_style_f32(UI_OUTSET_SHADOW_WIDTH, 2);
        ui_style_vec4(UI_OUTSET_SHADOW_COLOR, vec4(0, 0, 0, .4));
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, 120, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 40, 1});
    }

    ui_style_rule(".button.hover") {
        ui_style_vec4(UI_BG_COLOR, hsva2rgba(vec4(.8, .4, 1, 1.f)));
        ui_style_vec4(UI_BG_COLOR2, hsva2rgba(vec4(.8, .4, .6, 1.f)));
    }

    ui_style_rule(".button.focus") {
        ui_style_vec4(UI_BORDER_WIDTHS, vec4(2.5, 2.5, 2.5, 2.5));
        ui_style_vec4(UI_BORDER_COLOR, vec4(1, 1, 1, .6));
    }

    ui_style_rule(".button.press") {
        ui_style_vec4(UI_BG_COLOR, hsva2rgba(vec4(.8, .4, .6, .8f)));
        ui_style_vec4(UI_BG_COLOR2, hsva2rgba(vec4(.8, .4, 1, .8f)));
        ui_style_f32(UI_OUTSET_SHADOW_WIDTH, 0);
        ui_style_vec4(UI_OUTSET_SHADOW_COLOR, vec4(0, 0, 0, 0));
        ui_style_f32(UI_INSET_SHADOW_WIDTH, 2);
        ui_style_vec4(UI_INSET_SHADOW_COLOR, vec4(0, 0, 0, .4));
    }

    ui_style_rule("#box1 .button:last") {
        ui_style_vec4(UI_BG_COLOR,  hsva2rgba(vec4(.4, .4, 1, .8f)));
        ui_style_vec4(UI_BG_COLOR2, hsva2rgba(vec4(.4, .4, .6, .8f)));
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, 40, 0});
    }

    ui_style_rule(".vbox") {
        ui_style_vec2(UI_PADDING, vec2(8, 8));
        ui_style_f32(UI_SPACING, 8.0);
        ui_style_u32(UI_AXIS, UI_AXIS_VERTICAL);
        ui_style_vec4(UI_BG_COLOR, vec4(1, 1, 1, .08));
        ui_style_vec4(UI_BORDER_COLOR, vec4(0, 0, 0, .9));
        ui_style_f32(UI_EDGE_SOFTNESS, 0);
    }

    ui_style_rule(".hbox") {
        ui_style_vec2(UI_PADDING, vec2(8, 8));
        ui_style_f32(UI_SPACING, 8.0);
        ui_style_u32(UI_AXIS, UI_AXIS_HORIZONTAL);
        ui_style_vec4(UI_BG_COLOR, vec4(0, 0, 0, .2));
        ui_style_vec4(UI_BORDER_COLOR, vec4(0, 0, 0, .9));
        ui_style_f32(UI_EDGE_SOFTNESS, 0);
    }

    ui_style_rule(".hbox.item") {
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_CHILDREN_SUM, 0, 0});
        ui_style_vec4(UI_BORDER_WIDTHS, vec4(1, 1, 1, 1));
    }

    ui_box(0, "sub_root") {
        ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PCT_PARENT, 1, 0});
        ui_style_f32(UI_SPACING, 0);
        ui_style_vec2(UI_PADDING, vec2(0, 0));

        static Bool show_main_view = true;
        static Bool overlay_shown = false;

        ui_box(0, "box1") {
            ui_tag("vbox");
            ui_style_vec4(UI_BORDER_WIDTHS, vec4(1, 0, 0, 0));
            ui_style_size(UI_WIDTH, (UiSize){.tag=UI_SIZE_PCT_PARENT, .value=1./4});
            ui_style_size(UI_HEIGHT, (UiSize){.tag=UI_SIZE_PCT_PARENT, .value=1});

            if (ui_button("Foo1")->signal.clicked) {
                overlay_shown = !overlay_shown;
            }

            if (overlay_shown) {
                overlay_shown = show_modal();
            }

            ui_style_rule("#Foo2") { ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PCT_PARENT, 1, 0}); }
            ui_style_rule("#Foo3") { ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, 80, 0}); }

            UiBox *foo2 = ui_button("Foo2");
            UiBox *foo3 = ui_button("Foo3");

            if (foo2->signal.clicked) show_main_view = true;
            if (foo3->signal.clicked) show_main_view = false;

            if (show_main_view) {
                ui_tag_box(foo2, "press");
            } else {
                ui_tag_box(foo3, "press");
            }

            ui_vspacer();

            ui_box(UI_BOX_INVISIBLE, "bottom_sidebar_button") {
                ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_CHILDREN_SUM, 1, 1});
                ui_button("bar");
            }
        }

        if (show_main_view) {
            build_main_view();
        } else {
            build_second_view();
        }
    }
}

static Void update_input_state (Event *event) {
    ui->event = event;

    switch (event->tag) {
    case EVENT_DUMMY:       break;
    case EVENT_EATEN:       break;
    case EVENT_SCROLL:      break;
    case EVENT_WINDOW_SIZE: break;
    case EVENT_KEY_PRESS:   map_add(&ui->pressed_keys, event->key, 0); break;
    case EVENT_KEY_RELEASE: map_remove(&ui->pressed_keys, event->key); break;
    case EVENT_MOUSE_MOVE:
        ui->mouse_dt.x = event->x - ui->mouse.x;
        ui->mouse_dt.y = event->y - ui->mouse.y;
        ui->mouse.x = event->x;
        ui->mouse.y = event->y;
        break;
    }
}

static Void find_next_focus () {
    U64 start = ui->focus_idx;
    while (true) {
        ui->focus_idx = (ui->focus_idx + 1) % ui->depth_first.count;
        ui->focused = array_get(&ui->depth_first, ui->focus_idx);
        if (ui->focused->flags & UI_BOX_CAN_FOCUS) break;
        if (ui->focus_idx == start) break;
    }
}

static Void find_prev_focus () {
    U64 start = ui->focus_idx;
    while (true) {
        ui->focus_idx = (ui->focus_idx - 1);
        if (ui->focus_idx == UINT64_MAX) ui->focus_idx = ui->depth_first.count - 1;
        ui->focused = array_get(&ui->depth_first, ui->focus_idx);
        if (ui->focused->flags & UI_BOX_CAN_FOCUS) break;
        if (ui->focus_idx == start) break;
    }
}

static Void ui_frame (F32 dt) {
    ui->dt = dt;

    array_iter (event, &events, *) {
        update_input_state(event);

        UiRect *root_clip = array_ref_last(&ui->clip_stack);
        root_clip->w = win_width;
        root_clip->h = win_height;

        if (ui->depth_first.count) {
            if ((ui->event->tag == EVENT_KEY_PRESS) && (event->key == GLFW_KEY_TAB)) {
                if (event->mods == GLFW_MOD_SHIFT) find_prev_focus();
                else                               find_next_focus();
            }
        }

        ui->depth_first.count = 0;

        ui->root = ui_box(0, "root") {
            ui_style_size(UI_WIDTH, (UiSize){UI_SIZE_PIXELS, win_width, 0});
            ui_style_size(UI_HEIGHT, (UiSize){UI_SIZE_PIXELS, win_height, 0});
            ui_style_vec2(UI_PADDING, vec2(0, 0));
            ui_style_f32(UI_SPACING, 0);
            ui->root->rect.w = win_width;
            ui->root->rect.h = win_height;

            build();
        }

        // Remove unused boxes from the cache.
        map_iter (slot, &ui->box_cache) {
            Auto box = slot->val;
            if (box->gc_flag != ui->gc_flag) {
                if (box == ui->active) ui->active = 0;
                array_push(&ui->free_boxes, box);

                // @todo This should be officially supported by the map.
                slot->hash = MAP_HASH_OF_TOMB_ENTRY;
                ui->box_cache.umap.count--;
                ui->box_cache.umap.tomb_count++;
                MAP_IDX--;
            }
        }

        ui->gc_flag = !ui->gc_flag;
    }

    apply_style_rules();
    compute_layout();
    find_topmost_hovered_box(ui->root);
    render_box(ui->root);
}
