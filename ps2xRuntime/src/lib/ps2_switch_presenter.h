#pragma once

#if defined(PLATFORM_SWITCH) || defined(__SWITCH__)

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <iostream>

class SwitchGlesPresenter {
public:
    static SwitchGlesPresenter& Instance() {
        static SwitchGlesPresenter s_instance;
        return s_instance;
    }

    bool Initialize() {
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter::Initialize ENTER\n", 53);
        if (m_initialized) return true;
        NWindow* win = nwindowGetDefault();
        if (!win) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: nwindowGetDefault null\n", 58);
            std::cerr << "[SwitchGlesPresenter] Error: nwindowGetDefault() returned null\n";
            return false;
        }

        AppletOperationMode op_mode = appletGetOperationMode();
        m_winWidth = (op_mode == AppletOperationMode_Console) ? 1920 : 1280;
        m_winHeight = (op_mode == AppletOperationMode_Console) ? 1080 : 720;
        nwindowSetDimensions(win, m_winWidth, m_winHeight);
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: dimensions set\n", 51);

        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!m_display) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglGetDisplay failed\n", 56);
            std::cerr << "[SwitchGlesPresenter] eglGetDisplay failed\n";
            return false;
        }

        if (!eglInitialize(m_display, nullptr, nullptr)) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglInitialize failed\n", 56);
            std::cerr << "[SwitchGlesPresenter] eglInitialize failed\n";
            return false;
        }
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglInitialize SUCCESS\n", 57);

        if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
            std::cerr << "[SwitchGlesPresenter] eglBindAPI failed\n";
            return false;
        }

        static const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLConfig config;
        EGLint numConfigs = 0;
        if (!eglChooseConfig(m_display, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
            std::cerr << "[SwitchGlesPresenter] eglChooseConfig failed\n";
            return false;
        }

        m_surface = eglCreateWindowSurface(m_display, config, win, nullptr);
        if (!m_surface) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglCreateWindowSurface failed\n", 64);
            std::cerr << "[SwitchGlesPresenter] eglCreateWindowSurface failed\n";
            return false;
        }
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglCreateWindowSurface SUCCESS\n", 65);

        static const EGLint ctxAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, ctxAttribs);
        if (!m_context) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglCreateContext failed\n", 60);
            std::cerr << "[SwitchGlesPresenter] eglCreateContext failed\n";
            return false;
        }
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglCreateContext SUCCESS\n", 61);

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglMakeCurrent failed\n", 58);
            std::cerr << "[SwitchGlesPresenter] eglMakeCurrent failed\n";
            return false;
        }
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: eglMakeCurrent SUCCESS\n", 59);

        eglSwapInterval(m_display, 1);

        static const char* vsSrc =
            "attribute vec2 a_pos;\n"
            "attribute vec2 a_uv;\n"
            "varying vec2 v_uv;\n"
            "void main() {\n"
            "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
            "    v_uv = a_uv;\n"
            "}\n";
        static const char* fsSrc =
            "precision mediump float;\n"
            "varying vec2 v_uv;\n"
            "uniform sampler2D u_tex;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(u_tex, v_uv);\n"
            "}\n";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        if (!vs) return false;
        glShaderSource(vs, 1, &vsSrc, nullptr);
        glCompileShader(vs);
        GLint shaderStatus = GL_FALSE;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &shaderStatus);
        if (shaderStatus != GL_TRUE) {
            char log[512]{};
            GLsizei length = 0;
            glGetShaderInfoLog(vs, static_cast<GLsizei>(sizeof(log)), &length, log);
            std::cerr << "[SwitchGlesPresenter] Vertex shader compilation failed: " << log << '\n';
            if (length > 0) svcOutputDebugString(log, static_cast<size_t>(length));
            glDeleteShader(vs);
            return false;
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        if (!fs) {
            glDeleteShader(vs);
            return false;
        }
        glShaderSource(fs, 1, &fsSrc, nullptr);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &shaderStatus);
        if (shaderStatus != GL_TRUE) {
            char log[512]{};
            GLsizei length = 0;
            glGetShaderInfoLog(fs, static_cast<GLsizei>(sizeof(log)), &length, log);
            std::cerr << "[SwitchGlesPresenter] Fragment shader compilation failed: " << log << '\n';
            if (length > 0) svcOutputDebugString(log, static_cast<size_t>(length));
            glDeleteShader(vs);
            glDeleteShader(fs);
            return false;
        }

        m_program = glCreateProgram();
        if (!m_program) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return false;
        }
        glAttachShader(m_program, vs);
        glAttachShader(m_program, fs);
        glLinkProgram(m_program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint programStatus = GL_FALSE;
        glGetProgramiv(m_program, GL_LINK_STATUS, &programStatus);
        if (programStatus != GL_TRUE) {
            char log[512]{};
            GLsizei length = 0;
            glGetProgramInfoLog(m_program, static_cast<GLsizei>(sizeof(log)), &length, log);
            std::cerr << "[SwitchGlesPresenter] Shader link failed: " << log << '\n';
            if (length > 0) svcOutputDebugString(log, static_cast<size_t>(length));
            glDeleteProgram(m_program);
            m_program = 0;
            return false;
        }

        m_posAttr = glGetAttribLocation(m_program, "a_pos");
        m_uvAttr = glGetAttribLocation(m_program, "a_uv");
        m_texUniform = glGetUniformLocation(m_program, "u_tex");
        if (m_posAttr < 0 || m_uvAttr < 0 || m_texUniform < 0) {
            std::cerr << "[SwitchGlesPresenter] Required shader inputs are unavailable\n";
            glDeleteProgram(m_program);
            m_program = 0;
            return false;
        }

        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        std::vector<uint32_t> blank(640 * 512, 0xFF000000);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 640, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank.data());

        m_textureWidth = 640;
        m_textureHeight = 512;

        glGenBuffers(1, &m_vbo);

        m_initialized = true;
        svcOutputDebugString("[aoe2-switch] SwitchGlesPresenter: INITIALIZED FULLY SUCCESS\n", 62);
        std::cout << "[SwitchGlesPresenter] Initialized native EGL/GLES2 display ("
                  << m_winWidth << "x" << m_winHeight << ")\n";
        return true;
    }

    void Present(const void* rgbaPixels, uint32_t fbWidth, uint32_t fbHeight, uint32_t dispWidth, uint32_t dispHeight, uint32_t strideBytes) {
        if (!m_initialized) return;

        AppletOperationMode op_mode = appletGetOperationMode();
        u32 curWidth = (op_mode == AppletOperationMode_Console) ? 1920 : 1280;
        u32 curHeight = (op_mode == AppletOperationMode_Console) ? 1080 : 720;
        if (curWidth != m_winWidth || curHeight != m_winHeight) {
            m_winWidth = curWidth;
            m_winHeight = curHeight;
            nwindowSetDimensions(nwindowGetDefault(), m_winWidth, m_winHeight);
        }

        glViewport(0, 0, m_winWidth, m_winHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (rgbaPixels && fbWidth > 0 && fbHeight > 0) {
            glBindTexture(GL_TEXTURE_2D, m_texture);
            if (fbWidth != m_textureWidth || fbHeight != m_textureHeight) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbWidth, fbHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                m_textureWidth = fbWidth;
                m_textureHeight = fbHeight;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (strideBytes == uint64_t(fbWidth) * 4u) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fbWidth, fbHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
            } else if (strideBytes >= uint64_t(fbWidth) * 4u) {
                for (uint32_t y = 0; y < fbHeight; ++y)
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, fbWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                   static_cast<const uint8_t*>(rgbaPixels) + size_t(y) * strideBytes);
            }
        }

        float u_max = (fbWidth > 0) ? (static_cast<float>(dispWidth) / static_cast<float>(fbWidth)) : 1.0f;
        float v_max = (fbHeight > 0) ? (static_cast<float>(dispHeight) / static_cast<float>(fbHeight)) : 1.0f;

        float targetAspect = (dispHeight > 0) ? (static_cast<float>(dispWidth) / static_cast<float>(dispHeight)) : (4.0f / 3.0f);
        float winAspect = static_cast<float>(m_winWidth) / static_cast<float>(m_winHeight);
        float x_scale = 1.0f;
        float y_scale = 1.0f;
        if (winAspect > targetAspect) {
            x_scale = targetAspect / winAspect;
        } else {
            y_scale = winAspect / targetAspect;
        }

        float quadVertices[] = {
            -x_scale, -y_scale,  0.0f, v_max,
             x_scale, -y_scale,  u_max, v_max,
            -x_scale,  y_scale,  0.0f, 0.0f,
             x_scale,  y_scale,  u_max, 0.0f,
        };

        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glUniform1i(m_texUniform, 0);

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(m_posAttr);
        glVertexAttribPointer(m_posAttr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        glEnableVertexAttribArray(m_uvAttr);
        glVertexAttribPointer(m_uvAttr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(m_posAttr);
        glDisableVertexAttribArray(m_uvAttr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        static uint32_t s_presentCount = 0;
        static uint32_t s_fpsFrames = 0;
        static uint64_t s_lastFpsTick = 0;
        s_presentCount++;
        s_fpsFrames++;
        const uint64_t nowTick = armGetSystemTick();
        const uint64_t freqTick = armGetSystemTickFreq();
        if (s_lastFpsTick == 0) {
            s_lastFpsTick = nowTick;
        } else if (nowTick - s_lastFpsTick >= freqTick) {
            static const bool s_logFps = (std::getenv("AOE2_LOG_FPS") != nullptr);
            if (s_logFps) {
                const float currentFps = static_cast<float>(s_fpsFrames * freqTick) / static_cast<float>(nowTick - s_lastFpsTick);
                char fpsMsg[64];
                int msgLen = snprintf(fpsMsg, sizeof(fpsMsg), "[aoe2-switch] FPS: %.1f\n", currentFps);
                if (msgLen > 0) svcOutputDebugString(fpsMsg, msgLen);
            }
            s_fpsFrames = 0;
            s_lastFpsTick = nowTick;
        }
        eglSwapBuffers(m_display, m_surface);
        if (s_presentCount <= 3 || s_presentCount % 60 == 0) {
            svcOutputDebugString("[aoe2-switch] Present: after eglSwapBuffers\n", 44);
        }
    }

    void Shutdown() {
        if (!m_initialized) return;
        if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
        if (m_texture) { glDeleteTextures(1, &m_texture); m_texture = 0; }
        if (m_program) { glDeleteProgram(m_program); m_program = 0; }
        if (m_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
            if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
            eglTerminate(m_display);
            m_display = EGL_NO_DISPLAY;
            m_context = EGL_NO_CONTEXT;
            m_surface = EGL_NO_SURFACE;
        }
        m_initialized = false;
    }

private:
    SwitchGlesPresenter() = default;
    ~SwitchGlesPresenter() { Shutdown(); }

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    GLuint m_program = 0;
    GLuint m_texture = 0;
    GLuint m_vbo = 0;
    GLint m_posAttr = -1;
    GLint m_uvAttr = -1;
    GLint m_texUniform = -1;
    u32 m_winWidth = 1280;
    u32 m_winHeight = 720;
    uint32_t m_textureWidth = 640, m_textureHeight = 512;
    bool m_initialized = false;
};

#endif // PLATFORM_SWITCH
