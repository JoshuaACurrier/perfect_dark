// Post-processing: ambient occlusion and bloom. Included by gfx_opengl.cpp.
//
// Both run on the 3D scene before the HUD is drawn (the game marks that point with
// gDPPostFxEXT). The scene is in the game framebuffer, which may be multisampled:
//
// - Ambient occlusion (SSAO): the depth buffer is copied to a texture; a shader works out,
//   for every pixel, how much nearby geometry closes it in (a corner, the foot of a wall,
//   under a railing) and the scene is darkened there. View space positions come from the
//   depth and the game's projection matrix; normals from the depth's slope.
// - Bloom: the colour buffer is copied; its bright parts are blurred at quarter size and
//   added back, so lights, muzzle flashes and explosions glow.
//
// Needs GL 3.0 (depth textures in framebuffers, blits). If anything is missing it turns
// itself off and says so in the log.

int gfx_postfx_ssao = 1;            // Video.AmbientOcclusion
float gfx_postfx_ssao_strength = 1.f;
int gfx_postfx_bloom = 1;           // Video.Bloom
float gfx_postfx_bloom_strength = 1.f;

static struct {
    bool tried, ok;
    GLuint vao, vbo;
    GLuint prg_ao, prg_aoblend, prg_bright, prg_blur, prg_add;
    GLuint fbo_scene, tex_scene, tex_depth;
    GLuint fbo_ao, tex_ao;
    GLuint fbo_bloom[2], tex_bloom[2];
    uint32_t width, height;
} pfx;

static const char *pfx_vs_src =
    "in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aPos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// uProj: P[0][0], P[1][1], P[2][2], P[3][2], all divided by |P[2][3]| (the game scales its
// projection matrices); uProj2: P[2][3] / |P[2][3]|, P[3][3] / |P[2][3]|.
// uView: the player's viewport in the framebuffer, as offset and size in UV.
static const char *pfx_fs_common =
    "uniform sampler2D uDepth;\n"
    "uniform vec4 uProj;\n"
    "uniform vec2 uProj2;\n"
    "uniform vec4 uView;\n"
    "uniform vec2 uTexel;\n"
    "float viewW(float d) {\n"
    "    float ndc = d * 2.0 - 1.0;\n"
    "    float zv = (uProj.w - ndc * uProj2.y) / (ndc * uProj2.x - uProj.z);\n"
    "    return zv * uProj2.x + uProj2.y;\n"
    "}\n"
    "vec3 viewPos(vec2 uv) {\n"
    "    float w = viewW(texture(uDepth, uv).r);\n"
    "    vec2 ndc = (uv - uView.xy) / uView.zw * 2.0 - 1.0;\n"
    "    return vec3(ndc.x * w / uProj.x, ndc.y * w / uProj.y, w);\n"
    "}\n";

static const char *pfx_fs_ao =
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform float uRadius;\n"
    "uniform float uStrength;\n"
    "const int NUM = 12;\n"
    "void main() {\n"
    "    float d = texture(uDepth, vUV).r;\n"
    "    if (d >= 0.99999) { fragColor = vec4(1.0); return; }\n"
    "    vec3 p = viewPos(vUV);\n"
    // the slope of the surface, from whichever neighbour on each axis is on the same surface
    "    vec3 pl = viewPos(vUV - vec2(uTexel.x, 0.0));\n"
    "    vec3 pr = viewPos(vUV + vec2(uTexel.x, 0.0));\n"
    "    vec3 pd = viewPos(vUV - vec2(0.0, uTexel.y));\n"
    "    vec3 pu = viewPos(vUV + vec2(0.0, uTexel.y));\n"
    "    vec3 dx = (abs(pr.z - p.z) < abs(p.z - pl.z)) ? (pr - p) : (p - pl);\n"
    "    vec3 dy = (abs(pu.z - p.z) < abs(p.z - pd.z)) ? (pu - p) : (p - pd);\n"
    "    vec3 n = normalize(cross(dx, dy));\n"
    "    if (n.z > 0.0) n = -n;\n" // towards the camera
    // radius on screen; keep it sane up close and far away
    "    vec2 ssr = uRadius * vec2(uProj.x, uProj.y) * 0.5 / p.z * uView.zw;\n"
    "    ssr = clamp(ssr, uTexel * 2.0, vec2(0.12));\n"
    "    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    float sum = 0.0;\n"
    "    for (int i = 0; i < NUM; i++) {\n"
    "        float t = (float(i) + 0.5) / float(NUM);\n"
    "        float ang = (float(i) * 2.399963 + noise * 6.2831853);\n"
    "        vec2 uv = vUV + vec2(cos(ang), sin(ang)) * sqrt(t) * ssr;\n"
    "        vec3 v = viewPos(uv) - p;\n"
    "        float vv = dot(v, v);\n"
    // how far the sample rises above this pixel's surface, as an angle (0..1), less a bias so
    // flat and distant surfaces don't shade themselves; samples further away count for less
    "        float c = (dot(v, n) - 0.012 * p.z - 0.6) / sqrt(vv + 0.0001);\n"
    "        sum += clamp(c - 0.08, 0.0, 1.0) * clamp(1.0 - vv / (uRadius * uRadius), 0.0, 1.0);\n"
    "    }\n"
    "    float ao = clamp(1.0 - uStrength * 1.7 * sum / float(NUM), 0.3, 1.0);\n"
    // fade out with distance: far geometry is in fog and its depth is too coarse
    "    ao = mix(ao, 1.0, smoothstep(2500.0, 5000.0, p.z));\n"
    "    fragColor = vec4(ao, ao, ao, 1.0);\n"
    "}\n";

// blurs the AO over the 4x4 block its noise repeats in, without crossing depth edges,
// and multiplies the scene by it (through the blend mode)
static const char *pfx_fs_aoblend =
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uAO;\n"
    "void main() {\n"
    "    float w0 = viewW(texture(uDepth, vUV).r);\n"
    "    float sum = 0.0, wsum = 0.0;\n"
    "    for (int y = -2; y < 2; y++) {\n"
    "        for (int x = -2; x < 2; x++) {\n"
    "            vec2 uv = vUV + (vec2(float(x), float(y)) + 0.5) * uTexel;\n"
    "            float w = viewW(texture(uDepth, uv).r);\n"
    "            float k = 1.0 / (1.0 + abs(w - w0) * 40.0 / max(w0, 1.0));\n"
    "            sum += texture(uAO, uv).r * k;\n"
    "            wsum += k;\n"
    "        }\n"
    "    }\n"
    "    float ao = sum / max(wsum, 0.0001);\n"
    "    fragColor = vec4(ao, ao, ao, 1.0);\n"
    "}\n";

// the bright parts of the scene, shrunk to a quarter
static const char *pfx_fs_bright =
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "void main() {\n"
    "    vec3 c = vec3(0.0);\n"
    "    c += texture(uTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb;\n"
    "    c += texture(uTex, vUV + uTexel * vec2( 1.0, -1.0)).rgb;\n"
    "    c += texture(uTex, vUV + uTexel * vec2(-1.0,  1.0)).rgb;\n"
    "    c += texture(uTex, vUV + uTexel * vec2( 1.0,  1.0)).rgb;\n"
    "    c *= 0.25;\n"
    "    float l = max(c.r, max(c.g, c.b));\n"
    "    fragColor = vec4(c * smoothstep(0.72, 1.0, l), 1.0);\n"
    "}\n";

static const char *pfx_fs_blur =
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uDir;\n" // one texel along the blur direction, times the spread
    "void main() {\n"
    "    vec3 c = texture(uTex, vUV).rgb * 0.227027;\n"
    "    c += (texture(uTex, vUV + uDir * 1.384615).rgb + texture(uTex, vUV - uDir * 1.384615).rgb) * 0.316216;\n"
    "    c += (texture(uTex, vUV + uDir * 3.230769).rgb + texture(uTex, vUV - uDir * 3.230769).rgb) * 0.070270;\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

static const char *pfx_fs_add =
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uStrength;\n"
    "void main() {\n"
    "    fragColor = vec4(texture(uTex, vUV).rgb * uStrength, 1.0);\n"
    "}\n";

static GLuint pfx_compile(const char *name, const char *fs_a, const char *fs_b) {
    char header[64];
    snprintf(header, sizeof(header), "#version %s\n%s", gl_glsl_version_str, gl_es ? "precision highp float;\n" : "");

    const char *vs_src[] = { header, pfx_vs_src };
    const char *fs_src[] = { header, fs_a, fs_b ? fs_b : "" };

    GLint ok = 0;
    char log[1024];

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 2, vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        sysLogPrintf(LOG_WARNING, "GL: postfx: vertex shader %s: %s", name, log);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 3, fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        sysLogPrintf(LOG_WARNING, "GL: postfx: fragment shader %s: %s", name, log);
        return 0;
    }

    GLuint prg = glCreateProgram();
    glAttachShader(prg, vs);
    glAttachShader(prg, fs);
    glBindAttribLocation(prg, 0, "aPos");
    glLinkProgram(prg);
    glGetProgramiv(prg, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(prg, sizeof(log), NULL, log);
        sysLogPrintf(LOG_WARNING, "GL: postfx: program %s: %s", name, log);
        return 0;
    }

    return prg;
}

static void pfx_make_texture(GLuint *tex, GLint internal, GLenum format, GLenum type, uint32_t w, uint32_t h, GLint filter) {
    if (!*tex) {
        glGenTextures(1, tex);
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool pfx_init(void) {
    if (pfx.tried) {
        return pfx.ok;
    }
    pfx.tried = true;
    pfx.ok = false;

    if (GLVersion.major < 3 || !gfx_framebuffers_enabled || !glad_glGenVertexArrays) {
        sysLogPrintf(LOG_WARNING, "GL: postfx: needs OpenGL 3.0 and framebuffer effects; ambient occlusion and bloom are off");
        return false;
    }

    pfx.prg_ao = pfx_compile("ao", pfx_fs_common, pfx_fs_ao);
    pfx.prg_aoblend = pfx_compile("aoblend", pfx_fs_common, pfx_fs_aoblend);
    pfx.prg_bright = pfx_compile("bright", pfx_fs_bright, NULL);
    pfx.prg_blur = pfx_compile("blur", pfx_fs_blur, NULL);
    pfx.prg_add = pfx_compile("add", pfx_fs_add, NULL);
    if (!pfx.prg_ao || !pfx.prg_aoblend || !pfx.prg_bright || !pfx.prg_blur || !pfx.prg_add) {
        sysLogPrintf(LOG_WARNING, "GL: postfx: shaders failed; ambient occlusion and bloom are off");
        return false;
    }

    // one big triangle over the screen, in its own vertex array so the renderer's is left alone
    static const float tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
    glGenVertexArrays(1, &pfx.vao);
    glBindVertexArray(pfx.vao);
    glGenBuffers(1, &pfx.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, pfx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glBindVertexArray(opengl_vao);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

    glGenFramebuffers(1, &pfx.fbo_scene);
    glGenFramebuffers(1, &pfx.fbo_ao);
    glGenFramebuffers(2, pfx.fbo_bloom);

    pfx.ok = true;
    sysLogPrintf(LOG_NOTE, "GL: postfx: ambient occlusion and bloom available");
    return true;
}

static bool pfx_resize(uint32_t w, uint32_t h) {
    if (pfx.width == w && pfx.height == h) {
        return true;
    }

    pfx_make_texture(&pfx.tex_scene, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, w, h, GL_LINEAR);
    pfx_make_texture(&pfx.tex_depth, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_scene);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pfx.tex_scene, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, pfx.tex_depth, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    pfx_make_texture(&pfx.tex_ao, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, w, h, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_ao);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pfx.tex_ao, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    for (int i = 0; i < 2; ++i) {
        pfx_make_texture(&pfx.tex_bloom[i], GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, max(w / 4, 1U), max(h / 4, 1U), GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_bloom[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pfx.tex_bloom[i], 0);
        ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!ok) {
        sysLogPrintf(LOG_WARNING, "GL: postfx: could not make its framebuffers; ambient occlusion and bloom are off");
        pfx.ok = false;
        return false;
    }

    pfx.width = w;
    pfx.height = h;
    return true;
}

static inline void pfx_draw(void) {
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Whether the effects want the game drawn to an offscreen framebuffer (they need its depth).
bool gfx_opengl_postfx_wanted(void) {
    return (gfx_postfx_ssao || gfx_postfx_bloom) && gfx_framebuffers_enabled && (!pfx.tried || pfx.ok);
}

// Applies the effects to the framebuffer being drawn to, inside the viewport (x, y, w, h in
// its pixels). P is the projection matrix the scene was drawn with. Leaves GL's program,
// blend, depth, viewport, scissor and texture bindings changed: the caller re-applies its own.
void gfx_opengl_postfx(const float P[4][4], int vx, int vy, int vw, int vh) {
    if (!(gfx_postfx_ssao || gfx_postfx_bloom) || current_framebuffer == 0 || current_framebuffer >= framebuffers.size()) {
        return;
    }

    Framebuffer& fb = framebuffers[current_framebuffer];
    if (!fb.has_depth_buffer || vw <= 0 || vh <= 0 || !pfx_init() || !pfx_resize(fb.width, fb.height)) {
        return;
    }

    const float s = fabsf(P[2][3]);
    const bool perspective = s > 0.0001f && fabsf(P[0][0]) > 0.0001f && fabsf(P[1][1]) > 0.0001f;
    const bool do_ao = gfx_postfx_ssao && perspective;
    const bool do_bloom = gfx_postfx_bloom != 0;
    if (!do_ao && !do_bloom) {
        return;
    }

    const float texel[2] = { 1.f / (float)fb.width, 1.f / (float)fb.height };
    const float view[4] = { (float)vx * texel[0], (float)vy * texel[1], (float)vw * texel[0], (float)vh * texel[1] };

    glBindVertexArray(pfx.vao);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);

    if (do_ao) {
        // the scene's depth, as a texture
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pfx.fbo_scene);
        glBlitFramebuffer(0, 0, fb.width, fb.height, 0, 0, fb.width, fb.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        const float proj[4] = { P[0][0] / s, P[1][1] / s, P[2][2] / s, P[3][2] / s };
        const float proj2[2] = { P[2][3] / s, P[3][3] / s };

        // occlusion
        glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_ao);
        glViewport(0, 0, fb.width, fb.height);
        glUseProgram(pfx.prg_ao);
        glBindTexture(GL_TEXTURE_2D, pfx.tex_depth);
        glUniform1i(glGetUniformLocation(pfx.prg_ao, "uDepth"), 0);
        glUniform4fv(glGetUniformLocation(pfx.prg_ao, "uProj"), 1, proj);
        glUniform2fv(glGetUniformLocation(pfx.prg_ao, "uProj2"), 1, proj2);
        glUniform4fv(glGetUniformLocation(pfx.prg_ao, "uView"), 1, view);
        glUniform2fv(glGetUniformLocation(pfx.prg_ao, "uTexel"), 1, texel);
        glUniform1f(glGetUniformLocation(pfx.prg_ao, "uRadius"), 45.f);
        glUniform1f(glGetUniformLocation(pfx.prg_ao, "uStrength"), gfx_postfx_ssao_strength);
        pfx_draw();

        // darken the scene by it
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glUseProgram(pfx.prg_aoblend);
        glUniform1i(glGetUniformLocation(pfx.prg_aoblend, "uDepth"), 0);
        glUniform1i(glGetUniformLocation(pfx.prg_aoblend, "uAO"), 1);
        glUniform4fv(glGetUniformLocation(pfx.prg_aoblend, "uProj"), 1, proj);
        glUniform2fv(glGetUniformLocation(pfx.prg_aoblend, "uProj2"), 1, proj2);
        glUniform4fv(glGetUniformLocation(pfx.prg_aoblend, "uView"), 1, view);
        glUniform2fv(glGetUniformLocation(pfx.prg_aoblend, "uTexel"), 1, texel);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, pfx.tex_ao);
        glActiveTexture(GL_TEXTURE0);
        pfx_draw();
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
    }

    if (do_bloom) {
        // the scene's colour (with the occlusion in it), as a texture
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pfx.fbo_scene);
        glBlitFramebuffer(0, 0, fb.width, fb.height, 0, 0, fb.width, fb.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        const uint32_t bw = max(fb.width / 4, 1U), bh = max(fb.height / 4, 1U);
        glViewport(0, 0, bw, bh);

        // bright parts
        glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_bloom[0]);
        glUseProgram(pfx.prg_bright);
        glBindTexture(GL_TEXTURE_2D, pfx.tex_scene);
        glUniform1i(glGetUniformLocation(pfx.prg_bright, "uTex"), 0);
        glUniform2fv(glGetUniformLocation(pfx.prg_bright, "uTexel"), 1, texel);
        pfx_draw();

        // blur: across then down, twice, the second time wider
        glUseProgram(pfx.prg_blur);
        glUniform1i(glGetUniformLocation(pfx.prg_blur, "uTex"), 0);
        const GLint dirloc = glGetUniformLocation(pfx.prg_blur, "uDir");
        // spread relative to a 480 line picture, so the glow is the same size at any resolution
        const float scale = max((float)bh / 120.f, 1.f);
        for (int pass = 0; pass < 2; ++pass) {
            const float spread = (pass == 0 ? 1.f : 2.2f) * scale;
            glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_bloom[1]);
            glBindTexture(GL_TEXTURE_2D, pfx.tex_bloom[0]);
            glUniform2f(dirloc, spread / (float)bw, 0.f);
            pfx_draw();
            glBindFramebuffer(GL_FRAMEBUFFER, pfx.fbo_bloom[0]);
            glBindTexture(GL_TEXTURE_2D, pfx.tex_bloom[1]);
            glUniform2f(dirloc, 0.f, spread / (float)bh);
            pfx_draw();
        }

        // add it to the scene
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glViewport(0, 0, fb.width, fb.height);
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(pfx.prg_add);
        glBindTexture(GL_TEXTURE_2D, pfx.tex_bloom[0]);
        glUniform1i(glGetUniformLocation(pfx.prg_add, "uTex"), 0);
        glUniform1f(glGetUniformLocation(pfx.prg_add, "uStrength"), 0.6f * gfx_postfx_bloom_strength);
        pfx_draw();
        glDisable(GL_BLEND);
    }

    // back to the renderer's objects; it re-applies the rest of its state
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    glBindVertexArray(opengl_vao);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_SCISSOR_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
    glUseProgram(0);
}
