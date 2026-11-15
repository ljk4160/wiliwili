//
// Created by fang on 2022/8/12.
//

#include <cstdlib>
#include <clocale>
#include <cstdio>
#include <cmath>
#include <pystring.h>
#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/time.hpp>

#include "utils/config_helper.hpp"
#include "utils/stage.hpp"
#include "utils/number_helper.hpp"
#include "utils/crash_helper.hpp"
#include "view/mpv_core.hpp"

#ifdef PS5
#include <cstdarg>
#include <cstdio>

// 诊断探针：stderr 无缓冲，进程被内核直接收走时这一段也不会丢
// （stdout 重定向到文件是全缓冲的，最后几行会跟着进程消失）。
// 默认打开，WILIWILI_PROBE=0 关闭。
namespace
{
bool probeOn()
{
    static int on = -1;
    if (on < 0) {
        const char *value = std::getenv("WILIWILI_PROBE");
        on                = (value && value[0] == '0') ? 0 : 1;
    }
    return on != 0;
}

void probe(const char *format, ...) __attribute__((format(printf, 1, 2)));

void probe(const char *format, ...)
{
    if (!probeOn()) return;
    va_list args;
    va_start(args, format);
    std::fputs("[probe] ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}


}  // namespace
#endif

#ifdef MPV_BUNDLE_DLL
mpvSetOptionStringFunc mpvSetOptionString;
mpvObservePropertyFunc mpvObserveProperty;
mpvCreateFunc mpvCreate;
mpvInitializeFunc mpvInitialize;
mpvTerminateDestroyFunc mpvTerminateDestroy;
mpvSetWakeupCallbackFunc mpvSetWakeupCallback;
mpvCommandStringFunc mpvCommandString;
mpvErrorStringFunc mpvErrorString;
mpvWaitEventFunc mpvWaitEvent;
mpvGetPropertyFunc mpvGetProperty;
mpvCommandAsyncFunc mpvCommandAsync;
mpvGetPropertyStringFunc mpvGetPropertyString;
mpvFreeNodeContentsFunc mpvFreeNodeContents;
mpvSetOptionFunc mpvSetOption;
mpvFreeFunc mpvFree;
mpvRenderContextCreateFunc mpvRenderContextCreate;
mpvRenderContextSetUpdateCallbackFunc mpvRenderContextSetUpdateCallback;
mpvRenderContextRenderFunc mpvRenderContextRender;
mpvRenderContextReportSwapFunc mpvRenderContextReportSwap;
mpvRenderContextUpdateFunc mpvRenderContextUpdate;
mpvRenderContextFreeFunc mpvRenderContextFree;
mpvClientApiVersionFunc mpvClientApiVersion;
#endif

#ifdef MPV_USE_FB
#ifdef PS4
#include "utils/ps4_mpv_shaders.hpp"
#endif
#if defined(USE_GLES2)
#define SHADER_VERSION "#version 100\n"
#define SHADER_PRECISION "precision mediump float;\n"
#elif defined(USE_GLES3)
#define SHADER_VERSION "#version 300 es\n"
#define SHADER_PRECISION "precision mediump float;\n"
#else
#define SHADER_VERSION "#version 150 core\n"
#define SHADER_PRECISION ""
#endif

const char *vertexShaderSource = SHADER_VERSION
#ifdef USE_GLES2
    "attribute vec3 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 TexCoord;\n"
#else
    "in vec3 aPos;\n"
    "in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
#endif
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos, 1.0);\n"
    "   TexCoord = aTexCoord;\n"
    "}\0";
const char *fragmentShaderSource = SHADER_VERSION SHADER_PRECISION
#ifdef USE_GLES2
    "varying vec2 TexCoord;\n"
#else
    "in vec2 TexCoord;\n"
    "out vec4 FragColor;\n"
#endif
    "uniform sampler2D ourTexture;\n"
    "uniform float Alpha;\n"
    "void main()\n"
    "{\n"
#ifdef USE_GLES2
    "   gl_FragColor = texture2D(ourTexture, TexCoord);\n"
    "   gl_FragColor.a = Alpha;\n"
#else
    "   FragColor = texture(ourTexture, TexCoord);\n"
    "   FragColor.a = Alpha;\n"
#endif
    "}\n\0";

#define CONCATENATE3(s1, s2, s3) s1##s2##s3
#define CONCATENATE2(s1, s2) s1##s2
// The status a shader and a program are asked about is not the same one.
#define checkGLError(type, id, status)                                         \
    {                                                                          \
        char *info = nullptr;                                                  \
        int length = 0;                                                        \
        CONCATENATE3(glGet, type, iv)(id, status, &length);                    \
        if (length > 0) {                                                      \
            info = (char *)malloc(length);                                     \
            if (info) {                                                        \
                CONCATENATE3(glGet, type, InfoLog)(id, length, &length, info); \
            }                                                                  \
        }                                                                      \
        if (info) {                                                            \
            brls::Logger::error("Failed to load the shader: {}", info);        \
            free(info);                                                        \
        } else {                                                               \
            brls::Logger::error("Failed to load the shader");                  \
        }                                                                      \
        CONCATENATE2(glDelete, type)(id);                                      \
    }

static GLuint createShader(GLint type, const char *source) {
    GLuint shader = glCreateShader(type);
#ifdef PS4
    glShaderBinary(1, &shader, 2, type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT : PS4_MPV_SHADER_FRAG,
                   type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT_LENGTH : PS4_MPV_SHADER_FRAG_LENGTH);
#else
    int success;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    // check for shader compile errors
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        checkGLError(Shader, shader, GL_COMPILE_STATUS);
        return 0;
    }
#endif
    return shader;
}

static GLuint linkProgram(GLuint s1, GLuint s2) {
    if (s1 == 0 || s2 == 0) return 0;
    GLuint program = glCreateProgram();
    int success;
    glAttachShader(program, s1);
    glAttachShader(program, s2);
    glLinkProgram(program);
    // check for linking errors
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        checkGLError(Program, program, GL_LINK_STATUS);
        return 0;
    }
    return program;
}
#endif

#ifdef BOREALIS_USE_DEKO3D
#include <borealis/platforms/switch/switch_video.hpp>
#elif defined(BOREALIS_USE_D3D11)
#include <borealis/platforms/driver/d3d11.hpp>
extern std::unique_ptr<brls::D3D11Context> D3D11_CONTEXT;
#elif defined(BOREALIS_USE_GXM)
#include <borealis/platforms/psv/psv_video.hpp>
#include <borealis/extern/nanovg/nanovg_gxm.h>
#elif defined(USE_GL2)
#undef glBindFramebuffer
#define glBindFramebuffer(a, b) void()
#endif

#if defined(__linux__) && defined(__GLFW__) && defined(USE_EGL)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>
#endif

static inline void check_error(int status) {
    if (status < 0) {
        brls::Logger::error("MPV ERROR ====> {}", mpvErrorString(status));
    }
}

#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
static void *get_proc_address(void *unused, const char *name) {
#ifdef __SDL2__
    SDL_GL_GetCurrentContext();
    return (void *)SDL_GL_GetProcAddress(name);
#else
    glfwGetCurrentContext();
    return (void *)glfwGetProcAddress(name);
#endif
}
#endif

static inline float aspectConverter(const std::string &value) {
    try {
        if (value.empty()) {
            return -1;
        } else if (pystring::count(value, ":")) {
            // 比例模式
            auto num = pystring::split(value, ":");
            if (num.size() != 2) return -1;
            return std::stof(num[0]) / std::stof(num[1]);
        } else {
            // 纯数字
            return std::stof(value);
        }
    } catch (const std::exception &e) {
        return -1;
    }
}

void MPVCore::on_update(void *self) {
    static std::atomic<bool> update_in_flight{false};
#ifdef PS5
    // 画面不来时只有两种可能：播放器没有帧可以给，或者没有人向它要帧。
    // 把两边都数下来，日志才能指出是哪一种。
    static std::atomic<long> updateCalls{0};
    static std::atomic<long> updateBusy{0};
    static std::atomic<long> updateFrame{0};
    static std::atomic<long> updateIdle{0};
    updateCalls += 1;
#endif
    if (update_in_flight.exchange(true)) {
#ifdef PS5
        updateBusy += 1;
#endif
        return;
    }
    brls::sync([]() {
        update_in_flight = false;
        uint64_t flags = mpvRenderContextUpdate(MPVCore::instance().getContext());
#ifdef PS5
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            updateFrame += 1;
        } else {
            updateIdle += 1;
        }
        {
            static double updateAt = 0;
            double updateNow = brls::getCPUTimeUsec() / 1000.0;
            if (updateNow - updateAt > 1000.0) {
                updateAt = updateNow;
                brls::Logger::info("mpv update: calls={} busy={} frame={} idle={}", (long)updateCalls, (long)updateBusy,
                                   (long)updateFrame, (long)updateIdle);
            }
        }
#endif
// 软件渲染时 PS5 在 draw() 里自己出像素；用 framebuffer 模式时必须在这里渲染进
// FBO，否则没有任何地方调用 mpvRenderContextRender（draw() 的 FB 分支只做贴图），
// 纹理永远是空的 —— 表现就是黑屏、或者卡住最后一帧。
#if defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11) || \
    (defined(PS5) && defined(MPV_SW_RENDER))
        // 直接绘制到屏幕上，需要屏幕每刷新一次绘制一次，在 MPVCore 的绘制函数内部处理
        (void)flags;
#else
        // 绘制到 FBO、纹理、buffer...上，在主循环结尾进行绘制，MPVCore 的绘制函数内直接绘制对应的 FBO、纹理、buffer...
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            MPVCore::instance().redraw = true;
#ifdef MPV_SW_RENDER
            if (!MPVCore::instance().pixels) return;
            static double swRenderMs = 0;
            static double swRenderAt = 0;
            double start = brls::getCPUTimeUsec() / 1000.0;
            mpvRenderContextRender(MPVCore::instance().mpv_context, MPVCore::instance().mpv_params);
            mpvRenderContextReportSwap(MPVCore::instance().mpv_context);
            swRenderMs += brls::getCPUTimeUsec() / 1000.0 - start;
            double now = brls::getCPUTimeUsec() / 1000.0;
            if (now - swRenderAt > 10000.0) {
                brls::Logger::info("mpv sw render: {:.1f} ms a second", swRenderMs);
                swRenderMs = 0;
                swRenderAt = now;
            }
#else
#ifdef PS5
            // The cost of putting the picture on the screen: the picture goes
            // through a software OpenGL, and this is where the time goes.
            static double mpvRenderMs = 0;
            static double mpvRenderAt = 0;
            double start = brls::getCPUTimeUsec() / 1000.0;
#endif
            stage::Marker marker("mpv render");
            mpvRenderContextRender(MPVCore::instance().mpv_context, MPVCore::instance().mpv_params);
#ifdef PS5
            mpvRenderMs += brls::getCPUTimeUsec() / 1000.0 - start;
            double now = brls::getCPUTimeUsec() / 1000.0;
            if (now - mpvRenderAt > 10000.0) {
                // 同时报丢帧和实际帧率：渲染耗时高说明瓶颈在贴图/缩放，丢帧多而
                // 渲染不忙说明瓶颈在解码。两个数一起看才分得清。
                brls::Logger::info("mpv render: {:.1f} ms a second | drop out={} dec={} fps={:.1f}", mpvRenderMs,
                                   (long)MPVCore::instance().getInt("frame-drop-count"),
                                   (long)MPVCore::instance().getInt("decoder-frame-drop-count"),
                                   MPVCore::instance().getDouble("estimated-vf-fps"));
                mpvRenderMs = 0;
                mpvRenderAt = now;
            }
#endif
#ifdef BOREALIS_USE_OPENGL
            glBindFramebuffer(GL_FRAMEBUFFER, MPVCore::instance().default_framebuffer);
            glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
#endif
            mpvRenderContextReportSwap(MPVCore::instance().mpv_context);
#ifdef PS5
            // The console's player keeps the picture in a texture of its own
            // and shows it later. A look at that texture tells whether the
            // frames arrive at all.
            static int probe = 0;
            if (++probe >= 30) {
                probe = 0;
                GLint width = 0, height = 0;
                glBindTexture(GL_TEXTURE_2D, MPVCore::instance().media_texture);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
                unsigned char pixel[4] = {0, 0, 0, 0};
                glBindFramebuffer(GL_READ_FRAMEBUFFER, MPVCore::instance().media_framebuffer);
                glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                brls::Logger::info("ps5 video: texture {}x{}, framebuffer {}, pixel {},{},{},{}", width, height,
                                    MPVCore::instance().media_framebuffer, pixel[0], pixel[1], pixel[2], pixel[3]);
            }
#endif
#endif
        }
#endif
    });
}

void MPVCore::on_wakeup(void *self) {
    brls::sync([]() { MPVCore::instance().eventMainLoop(); });
}

#if defined(MPV_BUNDLE_DLL)
template <typename Module, typename fnGetProcAddress>
void initMpvProc(Module dll, fnGetProcAddress pGetProcAddress) {
    mpvSetOptionString     = (mpvSetOptionStringFunc)pGetProcAddress(dll, "mpv_set_option_string");
    mpvObserveProperty     = (mpvObservePropertyFunc)pGetProcAddress(dll, "mpv_observe_property");
    mpvCreate              = (mpvCreateFunc)pGetProcAddress(dll, "mpv_create");
    mpvInitialize          = (mpvInitializeFunc)pGetProcAddress(dll, "mpv_initialize");
    mpvTerminateDestroy    = (mpvTerminateDestroyFunc)pGetProcAddress(dll, "mpv_terminate_destroy");
    mpvSetWakeupCallback   = (mpvSetWakeupCallbackFunc)pGetProcAddress(dll, "mpv_set_wakeup_callback");
    mpvCommandString       = (mpvCommandStringFunc)pGetProcAddress(dll, "mpv_command_string");
    mpvErrorString         = (mpvErrorStringFunc)pGetProcAddress(dll, "mpv_error_string");
    mpvWaitEvent           = (mpvWaitEventFunc)pGetProcAddress(dll, "mpv_wait_event");
    mpvGetProperty         = (mpvGetPropertyFunc)pGetProcAddress(dll, "mpv_get_property");
    mpvCommandAsync        = (mpvCommandAsyncFunc)pGetProcAddress(dll, "mpv_command_async");
    mpvGetPropertyString   = (mpvGetPropertyStringFunc)pGetProcAddress(dll, "mpv_get_property_string");
    mpvFreeNodeContents    = (mpvFreeNodeContentsFunc)pGetProcAddress(dll, "mpv_free_node_contents");
    mpvSetOption           = (mpvSetOptionFunc)pGetProcAddress(dll, "mpv_set_option");
    mpvFree                = (mpvFreeFunc)pGetProcAddress(dll, "mpv_free");
    mpvRenderContextCreate = (mpvRenderContextCreateFunc)pGetProcAddress(dll, "mpv_render_context_create");
    mpvRenderContextUpdate = (mpvRenderContextUpdateFunc)pGetProcAddress(dll, "mpv_render_context_update");
    mpvRenderContextFree   = (mpvRenderContextFreeFunc)pGetProcAddress(dll, "mpv_render_context_free");
    mpvRenderContextRender = (mpvRenderContextRenderFunc)pGetProcAddress(dll, "mpv_render_context_render");
    mpvRenderContextSetUpdateCallback =
        (mpvRenderContextSetUpdateCallbackFunc)pGetProcAddress(dll, "mpv_render_context_set_update_callback");
    mpvRenderContextReportSwap = (mpvRenderContextReportSwapFunc)pGetProcAddress(dll, "mpv_render_context_report_swap");
    mpvClientApiVersion        = (mpvClientApiVersionFunc)pGetProcAddress(dll, "mpv_client_api_version");
}
#endif

MPVCore::MPVCore() {
#if defined(MPV_BUNDLE_DLL)
    HMODULE hMpv = ::LoadLibraryW(L"libmpv-2.dll");
    if (!hMpv) {
        HRSRC hSrc   = ::FindResource(nullptr, "MPV", RT_RCDATA);
        HGLOBAL hRes = ::LoadResource(nullptr, hSrc);
        DWORD dwSize = ::SizeofResource(nullptr, hSrc);
        dll          = MemoryLoadLibrary(::LockResource(hRes), dwSize);
        ::FreeResource(hRes);

        brls::Logger::info("Load bundled libmpv-2.dll, size: {}", dwSize);
        initMpvProc(dll, MemoryGetProcAddress);
    } else {
        char dllPath[MAX_PATH];
        ::GetModuleFileNameA(hMpv, dllPath, sizeof(dllPath));

        brls::Logger::info("Load external `{}`", dllPath);
        initMpvProc(hMpv, GetProcAddress);
    }
#endif
    this->init();
    // Destroy mpv when application exit
    brls::Application::getExitDoneEvent()->subscribe([this]() {
        this->clean();
#ifdef MPV_SW_RENDER
        if (pixels) {
            free(pixels);
            pixels             = nullptr;
            mpv_params[3].data = nullptr;
        }
#endif
    });
}

void MPVCore::init() {
    setlocale(LC_NUMERIC, "C");
    this->mpv = mpvCreate();
    if (!mpv) {
        brls::fatal("Error Create mpv Handle");
    }
    std::string confDir = ProgramConfig::instance().getConfigDir();
    // misc
    mpvSetOptionString(mpv, "config", "yes");
    mpvSetOptionString(mpv, "config-dir", confDir.c_str());
    mpvSetOptionString(mpv, "gpu-shader-cache-dir", fmt::format("{}/cache", confDir).c_str());
    mpvSetOptionString(mpv, "ytdl", "no");
    mpvSetOptionString(mpv, "audio-channels", "stereo");
    mpvSetOptionString(mpv, "idle", "yes");
    mpvSetOptionString(mpv, "loop-file", "no");
    mpvSetOptionString(mpv, "osd-level", "0");
    mpvSetOptionString(mpv, "video-timing-offset", "0");  // 60fps
    mpvSetOptionString(mpv, "keep-open", "yes");
    mpvSetOptionString(mpv, "hr-seek", "yes");
    mpvSetOptionString(mpv, "reset-on-next-file", "speed,pause");
    mpvSetOptionString(mpv, "vo", "libmpv");
    mpvSetOptionString(mpv, "pulse-latency-hacks", "no");

    mpvSetOption(mpv, "brightness", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_BRIGHTNESS);
    mpvSetOption(mpv, "contrast", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_CONTRAST);
    mpvSetOption(mpv, "saturation", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_SATURATION);
    mpvSetOption(mpv, "hue", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_HUE);
    mpvSetOption(mpv, "gamma", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_GAMMA);

#ifdef PS5
    // 不要开 opengl-pbo / vd-lavc-dr：这个驱动的 PBO 和 DR 是模拟实现，
    // 实测比同步上传慢一个数量级（884x1920 的小视频都能吃 4900ms/s，
    // 关掉后同样大小只要一百多毫秒）。
    // 这台机器的 GL 实现在 4K->1080p 降采样上开销极大：默认的 lanczos 每帧要
    // 110ms（60fps 下就是 6 倍实时，表现为疯狂丢帧）。双线性只要几毫秒，而 4K
    // 降采样用双线性的画质损失肉眼几乎看不出来。
    mpvSetOptionString(mpv, "dscale", "bilinear");
    mpvSetOptionString(mpv, "scale", "bilinear");
    mpvSetOptionString(mpv, "cscale", "bilinear");
    mpvSetOptionString(mpv, "correct-downscaling", "no");
    mpvSetOptionString(mpv, "linear-downscaling", "no");
    mpvSetOptionString(mpv, "dither", "no");
#endif

    if (MPVCore::LOW_QUALITY) {
        // Less cpu cost
        brls::Logger::info("lavc: skip loop filter and set fast decode");
        mpvSetOptionString(mpv, "vd-lavc-skiploopfilter", "all");
        mpvSetOptionString(mpv, "vd-lavc-fast", "yes");
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 2)) {
            mpvSetOptionString(mpv, "profile", "fast");
        } else {
            mpvSetOptionString(mpv, "scale", "bilinear");
            mpvSetOptionString(mpv, "dscale", "bilinear");
            mpvSetOptionString(mpv, "dither", "no");
            mpvSetOptionString(mpv, "correct-downscaling", "no");
            mpvSetOptionString(mpv, "linear-downscaling", "no");
            mpvSetOptionString(mpv, "sigmoid-upscaling", "no");
            mpvSetOptionString(mpv, "hdr-compute-peak", "no");
            mpvSetOptionString(mpv, "allow-delayed-peak-detect", "yes");
        }
    }

    if (MPVCore::INMEMORY_CACHE) {
        // cache
        brls::Logger::info("set memory cache: {}MB", MPVCore::INMEMORY_CACHE);
        mpvSetOptionString(mpv, "demuxer-max-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE).c_str());
        mpvSetOptionString(mpv, "demuxer-max-back-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE / 2).c_str());
    } else {
        mpvSetOptionString(mpv, "cache", "no");
    }

    // hardware decoding
#if defined(PS5)
    // The console has a video chip of its own: the decoder that runs on it
    // is part of the player, and mpv asks for it by name. The pictures come
    // out of it as they are decoded.
    if (HARDWARE_DEC) {
        mpvSetOptionString(mpv, "vd", PLAYER_HWDEC_METHOD.c_str());
        brls::Logger::info("MPV video decoder: {}", PLAYER_HWDEC_METHOD);
    } else {
        mpvSetOptionString(mpv, "vd", "!h264_ps5,!hevc_ps5,auto");
        brls::Logger::info("MPV video decoder: auto (sw)");
    }
    // A verified HTTPS stream cannot be opened on this console: ffmpeg looks
    // for its trust anchors in a path the payload layout owns, which a title
    // does not have, and the stream then fails to load before a single byte is
    // read. The app's own requests run without verification for the same
    // reason, and a video CDN address is not something to authenticate.
    mpvSetOptionString(mpv, "tls-verify", "no");
    // Keep the player's own warnings and errors in the application log: when a
    // stream fails to load this is the only place that says why. The output and
    // decode stages are verbose so a playback that stalls says which stage it
    // stalled in, not just that the picture never arrived.
    mpvSetOptionString(mpv, "msg-level",
                       "cplayer=info,stream=debug,demux=debug,vd=debug,vo=debug,ao=debug");
#else
    if (HARDWARE_DEC) {
        mpvSetOptionString(mpv, "hwdec", PLAYER_HWDEC_METHOD.c_str());
        brls::Logger::info("MPV hardware decode: {}", PLAYER_HWDEC_METHOD);
    } else {
        mpvSetOptionString(mpv, "hwdec", "no");
    }
#endif

    // Making the loading process faster
#if defined(__SWITCH__)
    mpvSetOptionString(mpv, "vd-lavc-dr", "no");
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    // This should fix random crash, but I don't know why.
    mpvSetOptionString(mpv, "opengl-glfinish", "yes");
#elif defined(PS4)
    mpvSetOptionString(mpv, "vd-lavc-threads", "6");
#elif defined(PS5)
    // The picture goes through a software OpenGL on this console, and the
    // scaling there reads every pixel of the video on the processor: for a
    // 4K video that costs more than the scaling on the processor itself.
    // The processor scales the picture down first and the graphics part only
    // sees the smaller picture.
    // The title gets a limited memory pool on the console, and the kernel
    // starts refusing allocations once it is used up. mpv's default network
    // cache alone is 150 MiB, which does not fit next to the decoder and the
    // renderer.
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    mpvSetOptionString(mpv, "vd-lavc-fast", "yes");
    // Direct rendering hands the decoder buffers that the picture path maps
    // for the graphics part, and the first decoded frame then takes the
    // process down on this console. Let the decoder keep its own frames and
    // copy them out instead.
    mpvSetOptionString(mpv, "vd-lavc-dr", "no");
    // A precise seek decodes every frame between the keyframe and the target
    // before the picture can appear, and that burst right after a file opens
    // is where playback dies here. Seeking to the keyframe keeps the cost to
    // one decode, at the price of landing on the nearest keyframe.
    mpvSetOptionString(mpv, "hr-seek", "no");
    mpvSetOptionString(mpv, "demuxer-max-bytes", "32M");
    mpvSetOptionString(mpv, "demuxer-readahead-secs", "10");
    mpvSetOptionString(mpv, "cache-secs", "30");
    mpvSetOptionString(mpv, "vd-queue-enable", "no");
    mpvSetOptionString(mpv, "audio-threads", "1");
    mpvSetOptionString(mpv, "demuxer-threads", "0");
    mpvSetOptionString(mpv, "vd-spawn", "no");
    mpvSetOptionString(mpv, "sws-scaler", "fast-bilinear");
    mpvSetOptionString(mpv, "sws-fast", "yes");
#elif defined(__PSV__)
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    mpvSetOptionString(mpv, "fbo-format", "rgba8");

    // Fix vo_wait_frame() cannot be wakeup
    mpvSetOptionString(mpv, "video-latency-hacks", "yes");
#endif
    // 过低的值可能导致部分直播流无法正确播放
    mpvSetOptionString(mpv, "demuxer-lavf-analyzeduration", "0.4");
    mpvSetOptionString(mpv, "demuxer-lavf-probescore", "24");

    // log
    // mpvSetOptionString(mpv, "msg-level", "ffmpeg=trace");
    // mpvSetOptionString(mpv, "msg-level", "all=no");
#if defined(PS5)
    mpvSetOptionString(mpv, "terminal", "no");
    // Warnings and errors only: a player writing every line of a long video to
    // the console's storage fills it up, and the whole app then fails to start.
    mpvSetOptionString(mpv, "msg-level", "all=warn");
#endif
    if (MPVCore::TERMINAL) {
        mpvSetOptionString(mpv, "terminal", "yes");
        if (brls::Logger::getLogLevel() >= brls::LogLevel::LOG_DEBUG) {
            mpvSetOptionString(mpv, "msg-level", "all=v");
        }
    }

    if (mpvInitialize(mpv) < 0) {
        mpvTerminateDestroy(mpv);
        brls::fatal("Could not initialize mpv context");
    }


    // set observe properties
    // 让 mpv 自己的诊断进应用日志：换清晰度/换编码时"流打不开""解码器选不到"
    // 这类失败只有 mpv 知道，它不写日志就只能看到画面不动。
    check_error(mpv_request_log_messages(mpv, "info"));
    check_error(mpvObserveProperty(mpv, 1, "core-idle", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 2, "eof-reached", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 3, "duration", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 4, "playback-time", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 5, "cache-speed", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 6, "percent-pos", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 7, "paused-for-cache", MPV_FORMAT_FLAG));
    //    check_error(mpvObserveProperty(mpv, 8, "demuxer-cache-time", MPV_FORMAT_DOUBLE));
    //    check_error(mpvObserveProperty(mpv, 9, "demuxer-cache-state", MPV_FORMAT_NODE));
    check_error(mpvObserveProperty(mpv, 10, "speed", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 11, "volume", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 12, "pause", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 13, "playback-abort", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 14, "seeking", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 15, "hwdec-current", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 16, "path", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 17, "brightness", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 18, "contrast", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 19, "saturation", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 20, "gamma", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 21, "hue", MPV_FORMAT_DOUBLE));
    // 播放中视频的参数：只靠通知维护，界面不再直接向 mpv 询问（会等播放线程）
    check_error(mpvObserveProperty(mpv, 22, "video-codec", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 23, "video-params/pixelformat", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 24, "video-params/w", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 25, "video-params/h", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 26, "container-fps", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 27, "video-bitrate", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 28, "frame-drop-count", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 29, "avsync", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 30, "video-params/gamma", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 31, "video-params/primaries", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 32, "video-params/colormatrix", MPV_FORMAT_STRING));

    // init renderer params
#ifdef MPV_SW_RENDER
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_DEKO3D)
    int advanced_control{1};
    auto switchPlatform = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
    mpv_deko3d_init_params deko_init_params{switchPlatform->getDeko3dDevice()};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DEKO3D)},
                              {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &deko_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_D3D11)
    mpv_dxgi_init_params init_params{D3D11_CONTEXT->getDevice(), D3D11_CONTEXT->getSwapChain()};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DXGI)},
        {MPV_RENDER_PARAM_DXGI_INIT_PARAMS, &init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#elif defined(BOREALIS_USE_GXM)
    auto *video_context            = (brls::PsvVideoContext *)brls::Application::getPlatform()->getVideoContext();
    auto *window                   = video_context->getWindow();
    auto *vg                       = brls::Application::getNVGContext();
    mpv_gxm_init_params gxm_params = {
        .context        = window->context,
        .shader_patcher = window->shader_patcher,
        .buffer_index   = 0,
        .msaa           = SCE_GXM_MULTISAMPLE_4X,
    };

    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_GXM},
                                 {MPV_RENDER_PARAM_GXM_INIT_PARAMS, &gxm_params},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};

    if (mpv_fbo.render_target == nullptr) {
        int texture_width     = DISPLAY_WIDTH;
        int texture_height    = DISPLAY_HEIGHT;
        int texture_stride    = ALIGN(texture_width, 8);
        nvg_image             = nvgCreateImageRGBA(vg, texture_width, texture_height, 0, nullptr);
        NVGXMtexture *texture = nvgxmImageHandle(vg, nvg_image);

        NVGXMframebufferInitOptions framebufferOpts = {
            .display_buffer_count = 1,  // Must be 1 for custom FBOs
            .scenesPerFrame       = 1,
            .render_target        = texture,
            .color_format         = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
            .color_surface_type   = SCE_GXM_COLOR_SURFACE_LINEAR,
            .display_width        = texture_width,
            .display_height       = texture_height,
            .display_stride       = texture_stride,
        };
        NVGXMframebuffer *fbo = gxmCreateFramebuffer(&framebufferOpts);
        mpv_fbo.render_target = fbo->gxm_render_target;
        mpv_fbo.color_surface = &fbo->gxm_color_surfaces[0].surface;
        mpv_fbo.depth_stencil_surface = &fbo->gxm_depth_stencil_surface;
        mpv_fbo.w   = texture_width;
        mpv_fbo.h   = texture_height;
    }
#else
    int advanced_control{1};
    mpv_opengl_init_params gl_init_params{get_proc_address, nullptr};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
#if defined(GLFW_EXPOSE_NATIVE_X11)
                              {MPV_RENDER_PARAM_X11_DISPLAY, glfwGetX11Display()},
#endif
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
                              {MPV_RENDER_PARAM_WL_DISPLAY, glfwGetWaylandDisplay()},
#endif
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#endif

    if (mpvRenderContextCreate(&mpv_context, mpv, params) < 0) {
        mpvTerminateDestroy(mpv);
        brls::fatal("failed to initialize mpv GL context");
    }
#ifdef BOREALIS_USE_D3D11
    wiliwili::initCrashDump();
#endif
    brls::Logger::info("MPV Version: {}", mpvGetPropertyString(mpv, "mpv-version"));
    brls::Logger::info("FFMPEG Version: {}", mpvGetPropertyString(mpv, "ffmpeg-version"));
    command_async("set", "audio-client-name", APPVersion::getPackageName());
    setVolume(MPVCore::VIDEO_VOLUME);

    // set event callback
    mpvSetWakeupCallback(mpv, on_wakeup, this);
    // set render callback
    mpvRenderContextSetUpdateCallback(mpv_context, on_update, this);

    focusSubscription = brls::Application::getWindowFocusChangedEvent()->subscribe([this](bool focus) {
        static bool playing = false;
        static std::chrono::system_clock::time_point sleepTime{};
        if (focus) {
            // restore AUTO_PLAY
            AUTO_PLAY = ProgramConfig::instance().getBoolOption(SettingItem::PLAYER_AUTO_PLAY);
            // application is on top
            auto timeNow = std::chrono::system_clock::now();
            if (playing && timeNow < (sleepTime + std::chrono::seconds(120))) {
                resume();
            }
        } else {
            // application is sleep, save the current state
            playing   = isPlaying();
            sleepTime = std::chrono::system_clock::now();
            pause();
            // do not automatically play video
            AUTO_PLAY = false;
        }
    });

    brls::Application::getExitEvent()->subscribe([]() { disableDimming(false); });

    this->initializeVideo();
}

#ifdef MPV_BUNDLE_DLL
MPVCore::~MPVCore() { MemoryFreeLibrary(dll); }
#else
MPVCore::~MPVCore() = default;
#endif

void MPVCore::clean() {
    check_error(mpvCommandString(this->mpv, "quit"));

    brls::Application::getWindowFocusChangedEvent()->unsubscribe(focusSubscription);

    brls::Logger::info("uninitialize Video");
    this->uninitializeVideo();

    brls::Logger::info("trying free mpv context");
    if (this->mpv_context) {
        mpvRenderContextFree(this->mpv_context);
        this->mpv_context = nullptr;
    }

    brls::Logger::info("trying terminate mpv");
    if (this->mpv) {
        mpvTerminateDestroy(this->mpv);
        this->mpv = nullptr;
    }

#ifdef MPV_SW_RENDER
    // 画布和它的像素缓冲在 clean() 里保留下来：关掉硬解、换解码器都会走
    // init() -> clean() 这条路，如果这里把表面销毁，下一帧就要重建纹理，而
    // "重建渲染纹理"本身就会随机把进程带走。画布尺寸固定，留着无害。
    if (nvg_image_stale) {
        nvgDeleteImage(brls::Application::getNVGContext(), nvg_image_stale);
        nvg_image_stale = 0;
    }
    // 这里不释放 pixels，不删除 nvg_image，也不清 sw_size。
    this->is_dovi_mode = false;
#endif
}

void MPVCore::restart() {
    this->clean();
    this->init();
    setMirror(MPVCore::VIDEO_MIRROR);
    setShader(currentShaderProfile, currentShader, currentSetting, false);
    mpvCoreEvent.fire(MpvEventEnum::RESTART);

    // 如果正在播放视频时重启mpv，重启前后存在软硬解切，那么视频尺寸会不正确
    // 手动设置一次尺寸可以解决这个问题 (同 MPVCore::reset())
    setFrameSize(rect);
}

void MPVCore::uninitializeVideo() {
#ifdef MPV_USE_FB
    if (media_framebuffer != 0) glDeleteFramebuffers(1, &media_framebuffer);
    if (media_texture != 0) glDeleteTextures(1, &media_texture);
    if (shader.vbo != 0) glDeleteBuffers(1, &shader.vbo);
    if (shader.ebo != 0) glDeleteBuffers(1, &shader.ebo);
    if (shader.prog != 0) glDeleteProgram(shader.prog);
#ifdef MPV_USE_VAO
    if (shader.vao != 0) glDeleteVertexArrays(1, &shader.vao);
    shader.vao = 0;
#endif
    media_framebuffer = 0;
    media_texture     = 0;
    shader.vbo        = 0;
    shader.ebo        = 0;
    shader.prog       = 0;
#endif
}

void MPVCore::initializeVideo() {
#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
    // Get default framebuffer
#if defined(IOS)
    // SDL: OpenGL ES on iOS doesn't use the traditional system-framebuffer setup provided in other operating systems.
    default_framebuffer = 1;
#else
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_framebuffer);
#endif
#endif

#if defined(MPV_NO_FB)
    mpv_fbo.fbo = default_framebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
#elif defined(MPV_USE_FB)
    if (this->media_texture != 0) return;
    brls::Logger::debug("initializeGL");

    // create texture
    // 视频渲染进这块纹理，再由下面的 shader 拉伸到窗口。上限 1080p：4K 的 RGBA
    // 纹理每帧要 33MB 带宽，1080p 只要 8.3MB，放大交给 GPU。
    int fboW = (int)brls::Application::windowWidth;
    int fboH = (int)brls::Application::windowHeight;
    if (fboW > 1920 || fboH > 1080) {
        const float scale = std::min(1920.0f / (float)fboW, 1080.0f / (float)fboH);
        fboW = (int)((float)fboW * scale);
        fboH = (int)((float)fboH * scale);
    }
    brls::Logger::info("mpv fbo: {}x{} (window {}x{})", fboW, fboH, (int)brls::Application::windowWidth,
                       (int)brls::Application::windowHeight);
    glGenTextures(1, &this->media_texture);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // create frame buffer
    glGenFramebuffers(1, &this->media_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, this->media_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->media_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        brls::Logger::error("glCheckFramebufferStatus failed");
        return;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    this->mpv_fbo.fbo = (int)this->media_framebuffer;
    // mpv 按这两个值决定往多大区域渲染，必须和纹理一致，否则只画一部分。
    this->mpv_fbo.w = fboW;
    this->mpv_fbo.h = fboH;
    brls::Logger::debug("create fbo and texture done\n");

    // build and compile our shader program
    // ------------------------------------
    // vertex shader
    GLuint vertexShader = createShader(GL_VERTEX_SHADER, vertexShaderSource);
    // fragment shader
    GLuint fragmentShader = createShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    // link shaders
    GLuint shaderProgram = linkProgram(vertexShader, fragmentShader);

    if (vertexShader) glDeleteShader(vertexShader);
    if (fragmentShader) glDeleteShader(fragmentShader);
    this->shader.prog = shaderProgram;

    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    unsigned int indices[] = {0, 1, 3, 1, 2, 3};
    glGenBuffers(1, &this->shader.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &this->shader.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

#ifdef MPV_USE_VAO
    glGenVertexArrays(1, &this->shader.vao);
    glBindVertexArray(this->shader.vao);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);

    GLuint aPos = glGetAttribLocation(shaderProgram, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    GLuint aTexCoord = glGetAttribLocation(shaderProgram, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
#endif
    brls::Logger::debug("initializeGL done");
#endif
}

void MPVCore::setFrameSize(brls::Rect r) {
    rect = r;
    if (std::isnan(rect.getWidth()) || std::isnan(rect.getHeight())) return;

#ifdef MPV_SW_RENDER
#ifdef BOREALIS_USE_D3D11
    // 使用 dx11 的拷贝交换，否则视频渲染异常
    const static int mpvImageFlags = NVG_IMAGE_STREAMING | NVG_IMAGE_COPY_SWAP;
#else
    const static int mpvImageFlags = 0;
#endif
    // 一块画布用到底：只在第一次进入时创建纹理，之后无论进全屏、退全屏、
    // 换清晰度、换视频都不销毁重建。
    // 这台机器上"重建渲染纹理"本身就会随机把进程带走（内核记 0xa0020328 /
    // 执行权限违规，信号不投递给应用，所以连崩溃报告都写不出来）。原代码注释
    // 里写了要"固定尺寸不重建纹理"，但实现只做了上限截断，每次 rect 变化照样
    // 重建，所以全屏和切清晰度都会崩。
    //
    // 画布固定 1080p：低分辨率视频由 GPU 放大，高分辨率视频缩小到 1080p 输出
    // （4K 每帧要 33MB memcpy，1080p 只要 8.3MB）。尺寸可用
    // /data/wiliwili/surface.txt 覆盖，内容形如 "1600x900"。
    static int canvasW = 0;
    static int canvasH = 0;
    if (canvasW == 0) {
        canvasW = 1920;
        canvasH = 1080;
        if (FILE *f = std::fopen("/data/wiliwili/surface.txt", "r")) {
            int w = 0, h = 0;
            if (std::fscanf(f, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 1920 && h <= 1080) {
                canvasW = (w + 15) & ~15;
                canvasH = (h + 1) & ~1;
            }
            std::fclose(f);
        }
#ifdef PS5
        probe("canvas %dx%d (固定画布)", canvasW, canvasH);
#endif
    }
    int drawWidth  = canvasW;
    int drawHeight = canvasH;
    const int maxW = canvasW;
    const int maxH = canvasH;

    VideoInfo vi = this->getVideoInfo();
    bool isHlg  = (vi.gamma == "arib-std-b67" || vi.gamma == "hlg" ||
                   (dovi_meta.meta[0] > 0.5f && dovi_meta.meta[1] > 0.5f));
    bool isDovi = !isHlg && (
        (dovi_meta.meta[0] > 0.5f && dovi_meta.meta[1] < 0.5f) ||
        vi.colormatrix.find("ipt") != std::string::npos ||
        vi.codec.find("dv") != std::string::npos ||
        vi.pixelFormat.find("dovi") != std::string::npos
    );

    // 画布只有一块：建好之后就不再重建，除非色彩格式（DoVi）变了。
    if (nvg_image != 0 && this->is_dovi_mode == isDovi) {
        return;
    }
    this->is_dovi_mode = isDovi;

    int requiredSize = isDovi ? (drawWidth * 2 * (drawHeight * 3 / 2)) : (drawWidth * drawHeight * PIXCEL_SIZE);

    if (pixels != nullptr && (size_t)requiredSize > allocated_size) {
        brls::Logger::debug("Enlarge video surface buffer to {}x{}", drawWidth, drawHeight);
        free(pixels);
        pixels = nullptr;
        allocated_size = 0;
    }

    if (pixels == nullptr) {
        // 按 256 行对齐、宽度多留 64 像素：mpv 写出的量有可能超过它声明的
        // sw_size 一点点，留出余量后越界只会落在自己的缓冲区里。
        const size_t rows = (size_t)((maxH + 255) & ~255);
        const size_t cols = (size_t)maxW + 64;
        allocated_size    = cols * rows * PIXCEL_SIZE;
        if (posix_memalign(&pixels, 64, allocated_size + 64) != 0) {
            pixels = nullptr;
            brls::Logger::error("Failed to allocate {} bytes for video surface buffer", allocated_size);
            allocated_size = 0;
            return;
        }
        std::memset(pixels, 0, allocated_size + 64);
        mpv_params[3].data = pixels;
#ifdef PS5
        probe("surface buffer %zu bytes for %dx%d (padded rows %zu)", allocated_size, maxW, maxH, rows);
#endif
    }

#ifdef PS5
    probe("setFrameSize in %dx%d (was %dx%d) nvg=%d paused=%d", drawWidth, drawHeight, sw_size[0], sw_size[1], nvg_image,
          (int)this->video_paused);
#endif
    // 先建新的，成功了再删旧的：中间不留"没有纹理"的空窗。原来的写法先删后建，
    // 一旦创建失败就永久停在 nvg_image == 0，视频再也不出来。
    auto *vg    = brls::Application::getNVGContext();
    int created = 0;
    if (isDovi) {
        sw_format = "p010";
        pitch     = drawWidth * 2;
        created   = nvgCreateImageR16(vg, drawWidth, drawHeight * 3 / 2, mpvImageFlags,
                                    (const unsigned char *)pixels);
    } else {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        sw_format = "abgr";
#else
        sw_format = "rgba";
#endif
        pitch   = PIXCEL_SIZE * drawWidth;
        created = nvgCreateImageRGBA(vg, drawWidth, drawHeight, mpvImageFlags, (const unsigned char *)pixels);
    }
    if (created == 0) {
        brls::Logger::error("nvgCreateImage failed for {}x{}", drawWidth, drawHeight);
        return;
    }
    if (nvg_image) {
        if (nvg_image_stale) nvgDeleteImage(vg, nvg_image_stale);
        nvg_image_stale = nvg_image;
    }
    nvg_image = created;
#ifdef PS5
    probe("setFrameSize out %dx%d nvg=%d", drawWidth, drawHeight, nvg_image);
#endif

    sw_size[0] = drawWidth;
    sw_size[1] = drawHeight;
    mpv_params[1].data = (void *)sw_format;
    mpv_params[2].data = &pitch;
    mpv_params[3].data = pixels;

    // 只有在视频暂停时调整纹理尺寸才需要强制渲染一次（视频画面会被清空为黑色）
    // 播放中绝对不能主动调用 mpvRenderContextRender，否则在视频切换/切分辨率时会导致滤镜链崩溃
    if (this->video_paused && !this->video_stopped) {
#ifdef PS5
        probe("setFrameSize force render in");
#endif
        mpvRenderContextRender(mpv_context, mpv_params);
#ifdef PS5
        probe("setFrameSize force render out");
#endif
        mpvRenderContextReportSwap(mpv_context);
        this->redraw = true;
    }
#elif !defined(MPV_USE_FB)
        // Using default framebuffer
#if defined(BOREALIS_USE_GXM)
    // This line will be called between beginFrame() and endFrame() in Application::frame(),
    // but mpvRenderContextRender(...) will call functions similar to beginFrame() and endFrame() to draw content to FBO,
    // and that will cause error in GXM, so call in brls::sync to make the mpv drawing calls outside the brls::Application::frame().
    brls::sync([this]() {
        mpvRenderContextRender(mpv_context, mpv_params);
        mpvRenderContextReportSwap(mpv_context);
    });
#elif defined(BOREALIS_USE_D3D11)
#else
    this->mpv_fbo.w = brls::Application::windowWidth;
    this->mpv_fbo.h = brls::Application::windowHeight;
#endif
    if (brls::Application::contentWidth < rect.getMaxX() || brls::Application::contentHeight < rect.getMaxY()) return;
    command_async("set", "video-margin-ratio-right",
                  (brls::Application::contentWidth - rect.getMaxX()) / brls::Application::contentWidth);
    command_async("set", "video-margin-ratio-bottom",
                  (brls::Application::contentHeight - rect.getMaxY()) / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-top", rect.getMinY() / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-left", rect.getMinX() / brls::Application::contentWidth);
#else
    if (this->media_texture == 0) return;
    int drawWidth  = rect.getWidth() * brls::Application::windowScale;
    int drawHeight = rect.getHeight() * brls::Application::windowScale;

    if (drawWidth == 0 || drawHeight == 0) return;
    brls::Logger::debug("MPVCore::setFrameSize: {}/{}", drawWidth, drawHeight);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, drawWidth, drawHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    this->mpv_fbo.w = drawWidth;
    this->mpv_fbo.h = drawHeight;

    float new_min_x = rect.getMinX() / brls::Application::contentWidth * 2 - 1;
    float new_min_y = 1 - rect.getMinY() / brls::Application::contentHeight * 2;
    float new_max_x = rect.getMaxX() / brls::Application::contentWidth * 2 - 1;
    float new_max_y = 1 - rect.getMaxY() / brls::Application::contentHeight * 2;

    vertices[0]  = new_max_x;
    vertices[1]  = new_min_y;
    vertices[5]  = new_max_x;
    vertices[6]  = new_max_y;
    vertices[10] = new_min_x;
    vertices[11] = new_max_y;
    vertices[15] = new_min_x;
    vertices[16] = new_min_y;

    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 在视频暂停时调整纹理尺寸，视频画面会被清空为黑色，强制重新绘制一次，避免这个问题
    mpvRenderContextRender(mpv_context, mpv_params);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
    mpvRenderContextReportSwap(mpv_context);
#endif
}

bool MPVCore::isValid() { return mpv_context != nullptr; }

void MPVCore::draw(brls::Rect area, float alpha) {
    if (mpv_context == nullptr) return;
#ifdef MPV_SW_RENDER
    // 上一帧的绘制指令已经 flush 完，这时才安全删掉退役的纹理。
    if (nvg_image_stale) {
        nvgDeleteImage(brls::Application::getNVGContext(), nvg_image_stale);
        nvg_image_stale = 0;
    }
#endif
    // The picture is put on the screen in two steps: the frame is resized to
    // the place the picture takes, and then the texture is drawn.
    stage::Marker marker("video draw");
    VideoInfo vi        = this->getVideoInfo();
#ifdef PS5
    {
        static long long lastW = -1, lastH = -1;
        if ((long long)vi.width != lastW || (long long)vi.height != lastH) {
            lastW = (long long)vi.width;
            lastH = (long long)vi.height;
            probe("video now %lldx%lld", lastW, lastH);
        }

        // 4K 的帧上传到这条 GL 要 85~100ms，1080p 只要 3ms（像素 4 倍、开销 30 倍）。
        // 所以超过 1080p 的输入先在 CPU 侧用 swscale 缩下来再上传：swscale 的 SIMD
        // 路径远比这条 GL 路径快，而画布本来也就是 1080p，画质没有额外损失。
        static int vfApplied  = 0;
        static int prescaleOn = -1;
        if (prescaleOn < 0) {
            // 默认开：原生 4K 的上传在这条 GL 路径上要 85~100ms/帧，必须先缩到
            // 1080p 才能跑满帧。想试原生 4K 就在 /data/wiliwili/prescale.txt 写 0。
            prescaleOn = 1;
            if (FILE *f = std::fopen("/data/wiliwili/prescale.txt", "r")) {
                int v = 0;
                if (std::fscanf(f, "%d", &v) == 1) prescaleOn = v != 0 ? 1 : 0;
                std::fclose(f);
            }
            brls::Logger::info("mpv: CPU 预缩放 {} (prescale.txt)", prescaleOn ? "开" : "关");
        }
        const bool wantScale = prescaleOn && ((vi.height > 1080) || (vi.width > 1920));
        if (wantScale && vfApplied == 0) {
            vfApplied = 1;
            command_async("vf", "set", "scale=-2:1080");
            brls::Logger::info("mpv: {}x{} 输入，先在 CPU 缩到 1080p 再上传", (long long)vi.width, (long long)vi.height);
        } else if (!wantScale && vfApplied == 1) {
            vfApplied = 0;
            command_async("vf", "set", "");
            brls::Logger::info("mpv: 输入不超过 1080p，去掉 CPU 缩放");
        }
    }
#endif
    [[maybe_unused]] bool isHlg          = (vi.gamma == "arib-std-b67" || vi.gamma == "hlg"
#ifdef MPV_SW_RENDER
                           || (dovi_meta.meta[0] > 0.5f && dovi_meta.meta[1] > 0.5f)
#endif
    );
    [[maybe_unused]] bool isDoviProfile5 = !isHlg && (
#ifdef MPV_SW_RENDER
        (dovi_meta.meta[0] > 0.5f && dovi_meta.meta[1] < 0.5f) ||
#endif
        vi.colormatrix.find("ipt") != std::string::npos ||
        vi.codec.find("dv") != std::string::npos ||
        vi.pixelFormat.find("dovi") != std::string::npos
    );

    if (!(this->rect == area)
#ifdef MPV_SW_RENDER
        // clean() 释放像素缓冲和纹理并把 sw_size 归零，而切硬解/切编码/换着色器
        // 都走这条路且 rect 不变：只比 rect 的话这里会永远跳过重建，draw() 每帧
        // 都在下面的判空处返回，画面就再也出不来了。
        || this->is_dovi_mode != isDoviProfile5 || pixels == nullptr || nvg_image == 0 ||
        (int)vi.width != this->surface_video_w || (int)vi.height != this->surface_video_h
#endif
    ) {
        setFrameSize(area);
    }

#ifdef MPV_SW_RENDER
    if (!pixels || nvg_image == 0) {
        static int warned = 0;
        if (warned++ < 3)
            brls::Logger::error("video surface is not ready (pixels={}, nvg_image={})", (void *)pixels, nvg_image);
        return;
    }

#ifdef PS5
    uint64_t updateFlags = mpvRenderContextUpdate(mpv_context);

    static double renderTotalMs = 0;
    static int renderCount = 0;
    if (updateFlags & MPV_RENDER_UPDATE_FRAME) {
        double rStart = brls::getCPUTimeUsec() / 1000.0;
        mpvRenderContextRender(mpv_context, mpv_params);
        double rCost = (brls::getCPUTimeUsec() / 1000.0) - rStart;
        renderTotalMs += rCost;
        renderCount++;
        mpvRenderContextReportSwap(mpv_context);
        this->redraw = true;
    }
    {
        static long drawCalls = 0, drawRedraws = 0;
        static double drawAt = 0;
        drawCalls += 1;
        if (this->redraw) drawRedraws += 1;
        double drawNow = brls::getCPUTimeUsec() / 1000.0;
        if (drawNow - drawAt > 10000.0) {
            drawAt = drawNow;
            double avgRenderMs = renderCount > 0 ? (renderTotalMs / renderCount) : 0;
            brls::Logger::debug("video draw (10s): calls={} redraw={} avgRender={:.1f}ms totalRender={:.1f}ms", 
                               drawCalls, drawRedraws, avgRenderMs, renderTotalMs);
            renderTotalMs = 0;
            renderCount = 0;
        }
    }
#endif

    auto *vg = brls::Application::getNVGContext();
    if (this->redraw) {
        this->redraw = false;
        static double nvgUpdateMs = 0;
        static double nvgUpdateAt = 0;
        double start = brls::getCPUTimeUsec() / 1000.0;
        nvgUpdateImage(vg, nvg_image, (const unsigned char *)pixels);
        nvgUpdateMs += brls::getCPUTimeUsec() / 1000.0 - start;
        double now = brls::getCPUTimeUsec() / 1000.0;
        if (now - nvgUpdateAt > 10000.0) {
            brls::Logger::debug("nvgUpdateImage (10s): {:.1f} ms", nvgUpdateMs);
            nvgUpdateMs = 0;
            nvgUpdateAt = now;
        }
    }

    // draw black background
    nvgBeginPath(vg);
    NVGcolor bg{};
    bg.a = alpha;
    nvgFillColor(vg, bg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgFill(vg);

    // draw video
    nvgBeginPath(vg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    NVGpaint videoPaint = nvgImagePattern(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight(), 0, nvg_image, alpha);
    bool isHdr = (vi.pixelFormat == "p010" ||
                  vi.pixelFormat.find("10") != std::string::npos ||
                  vi.gamma == "pq" ||
                  vi.gamma == "smpte2084" ||
                  isHlg ||
                  vi.primaries == "bt.2020");
    if (isHlg) {
        videoPaint.radius = 3.0f; // 触发 GPU 片段着色器 HLG BT.2100 -> PQ / SDR 电影级转换
    } else if (isDoviProfile5) {
#if defined(PS5) && defined(BOREALIS_USE_VULKAN)
        nvgSetDoviMetadata(vg, &dovi_meta);
#endif
        videoPaint.radius = 2.0f; // 触发 GPU 片段着色器 Dolby Vision Profile 5 RPU 重塑与色调映射
    } else if (isHdr) {
        videoPaint.radius = 1.0f; // 触发 GPU 片段着色器 HDR PQ BT.2020 -> SDR BT.709 电影级色调映射
    }
#if defined(PS5) && defined(BOREALIS_USE_VULKAN)
    if (!isDoviProfile5) {
        nvgSetDoviMetadata(vg, nullptr);
    }
#endif
    if (isDoviProfile5 || isHlg) {
        static int dovi_draw_log = 0;
        if (dovi_draw_log++ < 5 || (dovi_draw_log % 600 == 0)) {
            brls::Logger::debug("[DV_DRAW] isDovi={} isHlg={} isHdr={} curQ={} radius={:.1f} meta=({:.1f},{:.1f},{:.1f}) ycc0=({:.2f},{:.2f},{:.2f}) off=({:.2f},{:.2f},{:.2f}) gamma='{}' matrix='{}' pixfmt='{}'",
                isDoviProfile5, isHlg, isHdr, this->currentQuality, videoPaint.radius,
                dovi_meta.meta[0], dovi_meta.meta[1], dovi_meta.meta[2],
                dovi_meta.ycc[0][0], dovi_meta.ycc[0][1], dovi_meta.ycc[0][2],
                dovi_meta.off[0], dovi_meta.off[1], dovi_meta.off[2],
                vi.gamma, vi.colormatrix, vi.pixelFormat);
        }
    }
    nvgFillPaint(vg, videoPaint);
    nvgFill(vg);
#elif defined(BOREALIS_USE_GXM)
    auto *vg = brls::Application::getNVGContext();

    if (MPVCore::VIDEO_MIRROR) {
        nvgSave(vg);
        nvgTranslate(vg, area.getWidth() + area.getMinX() * 2, 0);
        nvgScale(vg, -1, 1);
    }
    NVGpaint img = nvgImagePattern(vg, 0, 0, brls::Application::contentWidth, brls::Application::contentHeight, 0,
                                   nvg_image, alpha);
    nvgBeginPath(vg);
    nvgRect(vg, area.getMinX(), area.getMinY(), area.getWidth(), area.getHeight());
    nvgFillPaint(vg, img);
    nvgFill(vg);
    if (MPVCore::VIDEO_MIRROR) {
        nvgRestore(vg);
    }
#elif defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11)
    // 只在非透明时绘制视频，可以避免退出页面时视频画面残留
    if (alpha >= 1) {
#ifdef BOREALIS_USE_DEKO3D
        static auto videoContext = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
        mpv_fbo.tex              = videoContext->getFramebuffer();
        videoContext->queueSignalFence(&readyFence);
        videoContext->queueFlush();
#endif
        mpvRenderContextRender(this->mpv_context, mpv_params);
#ifdef BOREALIS_USE_DEKO3D
        videoContext->queueWaitFence(&doneFence);
#elif defined(BOREALIS_USE_D3D11)
        D3D11_CONTEXT->beginFrame();
#else
        glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
        glViewport(0, 0, brls::Application::windowWidth, brls::Application::windowHeight);
#endif
        mpvRenderContextReportSwap(this->mpv_context);

        // 画背景来覆盖mpv的黑色边框
        if (rect.getWidth() < brls::Application::contentWidth) {
            auto *vg = brls::Application::getNVGContext();
            nvgBeginPath(vg);
            nvgFillColor(vg, brls::Application::getTheme().getColor("brls/background"));
            nvgRect(vg, 0, 0, rect.getMinX(), brls::Application::contentHeight);
            nvgRect(vg, rect.getMaxX(), 0, brls::Application::contentWidth - rect.getMaxX(),
                    brls::Application::contentHeight);
            nvgRect(vg, rect.getMinX() - 1, 0, rect.getWidth() + 2, rect.getMinY());
            nvgRect(vg, rect.getMinX() - 1, rect.getMaxY(), rect.getWidth() + 2,
                    brls::Application::contentHeight - rect.getMaxY());
            nvgFill(vg);
        }
    }
#else
    // OpenGL 将 FBO 绘制到屏幕上
    glUseProgram(shader.prog);
    // The sampler of the shader reads the first unit, so the picture is put
    // there: the unit that is active belongs to whoever drew before.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    static GLint textureID = glGetUniformLocation(shader.prog, "ourTexture");
    if (textureID >= 0) glUniform1i(textureID, 0);
    // The picture fills the area of the video: the scissors and the viewport
    // of the interface must not cut it.
    GLboolean scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    // 画面在屏幕上的位置由 setFrameSize() 算好的 NDC 顶点决定，这里必须用整个窗口
    // 的视口：视口再按 rect 缩小会变成二次变换，画面被放大并裁掉（表现为"画面不全"）。
    glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
    // The state of whoever drew before: the video is a flat picture in the
    // foreground, and the depth test and the culling of the faces would
    // throw it away.
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean cullFace  = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
#ifdef MPV_USE_VAO
    glBindVertexArray(shader.vao);
#else
    glBindBuffer(GL_ARRAY_BUFFER, shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shader.ebo);
    static GLuint aPos = glGetAttribLocation(shader.prog, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    static GLuint aTexCoord = glGetAttribLocation(shader.prog, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);
#endif

    // Set alpha
    if (alpha < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    static GLint alphaID = glGetUniformLocation(shader.prog, "Alpha");
    glUniform1f(alphaID, alpha);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    if (scissorTest) glEnable(GL_SCISSOR_TEST);
    if (depthTest) glEnable(GL_DEPTH_TEST);
    if (cullFace) glEnable(GL_CULL_FACE);
#endif
}

mpv_render_context *MPVCore::getContext() { return this->mpv_context; }

mpv_handle *MPVCore::getHandle() { return this->mpv; }

MPVEvent *MPVCore::getEvent() { return &this->mpvCoreEvent; }

std::string MPVCore::getCacheSpeed() const {
    if (cache_speed >> 20 > 0) {
        return fmt::format("{:.2f} MB/s", (cache_speed >> 10) / 1024.0f);
    } else if (cache_speed >> 10 > 0) {
        return fmt::format("{:.2f} KB/s", cache_speed / 1024.0f);
    } else {
        return fmt::format("{} B/s", cache_speed);
    }
}

void MPVCore::eventMainLoop() {
    while (true) {
        auto event = mpvWaitEvent(this->mpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_NONE:
                return;
            case MPV_EVENT_LOG_MESSAGE: {
                auto log = (mpv_event_log_message *)event->data;
                if (log->log_level <= MPV_LOG_LEVEL_ERROR) {
                    brls::Logger::error("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_WARN) {
                    brls::Logger::warning("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_INFO) {
                    brls::Logger::info("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_V) {
                    brls::Logger::debug("{}: {}", log->prefix, log->text);
                } else {
                    brls::Logger::verbose("{}: {}", log->prefix, log->text);
                }
            } break;
            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("========> MPV_EVENT_FILE_LOADED");
                // event 8: 文件预加载结束，准备解码
                mpvCoreEvent.fire(MpvEventEnum::MPV_LOADED);
                // 发布一次进度更新事件，避免进度条在0秒时没有进度更新
                video_progress = 0;
                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
#ifndef PS5
                // Clearing the playlist while the first entry plays makes the
                // player re-open the stream it is already playing, and on this
                // console that re-open is what takes the process down: the
                // picture never arrives. The spare addresses stay in the list
                // instead, which is harmless - looping is off, so the player
                // never advances to them by itself.
                command_async("playlist-clear");
#endif

                if (AUTO_PLAY) {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                    this->resume();
                } else {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                    this->pause();
                }
                break;
            case MPV_EVENT_START_FILE:
                // event 6: 开始加载文件
                brls::Logger::info("========> MPV_EVENT_START_FILE");
                {
                    std::lock_guard<std::mutex> lock(videoInfoMutex);
                    videoInfo = {};
                }
#ifdef MPV_SW_RENDER
                memset(&dovi_meta, 0, sizeof(dovi_meta));
#endif

                // show osd for a really long time
                mpvCoreEvent.fire(MpvEventEnum::START_FILE);

                mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                break;
#ifdef PS5
            case MPV_EVENT_VIDEO_RECONFIG:
                // event 17: 视频链重新配置（分辨率、像素格式变化）
                brls::Logger::info("========> MPV_EVENT_VIDEO_RECONFIG");
#ifdef MPV_SW_RENDER
                memset(&dovi_meta, 0, sizeof(dovi_meta));
#endif
                break;
            case MPV_EVENT_AUDIO_RECONFIG:
                // event 18: 音频链重新配置
                brls::Logger::info("========> MPV_EVENT_AUDIO_RECONFIG");
                break;
            case MPV_EVENT_SEEK:
                // event 20: 开始跳转
                brls::Logger::info("========> MPV_EVENT_SEEK");
                break;
#endif
            case MPV_EVENT_PLAYBACK_RESTART:
                // event 21: 开始播放文件（一般是播放或调整进度结束之后触发）
                brls::Logger::info("========> MPV_EVENT_PLAYBACK_RESTART");
                video_stopped = false;
                mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                break;
            case MPV_EVENT_END_FILE: {
                // event 7: 文件播放结束
                brls::Logger::info("========> MPV_STOP");
                mpvCoreEvent.fire(MpvEventEnum::MPV_STOP);
                video_stopped = true;
                auto node     = (mpv_event_end_file *)event->data;
                if (node->reason == MPV_END_FILE_REASON_ERROR) {
                    mpv_error_code = node->error;
                    brls::Logger::error("========> MPV ERROR: {}", mpvErrorString(node->error));
                    mpvCoreEvent.fire(MpvEventEnum::MPV_FILE_ERROR);
                }
#ifdef BOREALIS_USE_GXM
                else
                {
                    setFrameSize(rect); // 清空残留画面
                }
#endif

                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto *data = ((mpv_event_property *)event->data)->data;
                switch (event->reply_userdata) {
                    case 1:
                        if (data) {
                            bool playing = *(int *)data == 0;
                            if (playing != video_playing) {
                                video_playing = playing;
                                mpvCoreEvent.fire(MpvEventEnum::MPV_IDLE);
                            }
                            video_playing = playing;
                            disableDimming(video_playing);
                        }
                        break;
                    case 2:
                        if (data) video_eof = *(int *)data;
                        // 当视频播放自然结束时会先触发一次 EOF，如果这时 stop 或者设置了新的播放文件，会触发 STOP 然后再触发一次 EOF
                        // 避免多次触发 EOF，将第二种情况排除在外
                        if (video_eof && !video_stopped) {
                            brls::Logger::info("========> END OF FILE");
                            mpvCoreEvent.fire(MpvEventEnum::END_OF_FILE);
                        }
                        break;
                    case 3:
                        // 视频总时长更新
                        if (((mpv_event_property *)event->data)->data)
                            duration = *(int64_t *)((mpv_event_property *)event->data)->data;
                        if (duration != 0) {
                            brls::Logger::debug("========> duration: {}", duration);
                            mpvCoreEvent.fire(MpvEventEnum::UPDATE_DURATION);
                        }
                        break;
                    case 4:
                        // 播放进度更新
                        if (((mpv_event_property *)event->data)->data) {
                            playback_time = *(double *)((mpv_event_property *)event->data)->data;
                            if (video_progress != (int64_t)playback_time) {
                                video_progress = (int64_t)playback_time;
                                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
                                // 判断是否需要暂停播放
                                if (CLOSE_TIME > 0 && wiliwili::getUnixTime() > CLOSE_TIME) {
                                    CLOSE_TIME = 0;
                                    this->pause();
                                }
                            }
                        }

                        break;
                    case 5:
                        // 视频 cache speed
                        if (((mpv_event_property *)event->data)->data) {
                            cache_speed = *(int64_t *)((mpv_event_property *)event->data)->data;
                            mpvCoreEvent.fire(MpvEventEnum::CACHE_SPEED_CHANGE);
                        }
                        break;
                    case 6:
                        // 视频进度更新（百分比）
                        if (((mpv_event_property *)event->data)->data) {
                            percent_pos = *(double *)((mpv_event_property *)event->data)->data;
                        }
                        break;
                    case 7:
                        // 发生了缓存等待
                        if (!data) break;

                        if (*(int *)data) {
                            brls::Logger::info("========> VIDEO PAUSED FOR CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        } else {
                            brls::Logger::info("========> VIDEO RESUME FROM CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                        }
                        break;
                    case 8:
                        // 缓存时间
                        if (((mpv_event_property *)event->data)->data) {
                            brls::Logger::verbose("demuxer-cache-time: {}",
                                                  *(double *)((mpv_event_property *)event->data)->data);
                        }
                        break;
                    case 9:
                        // 缓存信息
                        if (((mpv_event_property *)event->data)->data) {
                            auto *node = (mpv_node *)((mpv_event_property *)event->data)->data;
                            std::unordered_map<std::string, mpv_node> node_map;
                            for (int i = 0; i < node->u.list->num; i++) {
                                node_map.insert(
                                    std::make_pair(std::string(node->u.list->keys[i]), node->u.list->values[i]));
                            }
                            brls::Logger::debug(
                                "total-bytes: {:.2f}MB; cache-duration: "
                                "{:.2f}; "
                                "underrun: {}; fw-bytes: {:.2f}MB; bof-cached: "
                                "{}; eof-cached: {}; file-cache-bytes: {}; "
                                "raw-input-rate: {:.2f};",
                                node_map["total-bytes"].u.int64 / 1048576.0, node_map["cache-duration"].u.double_,
                                node_map["underrun"].u.flag, node_map["fw-bytes"].u.int64 / 1048576.0,
                                node_map["bof-cached"].u.flag, node_map["eof-cached"].u.flag,
                                node_map["file-cache-bytes"].u.int64 / 1048576.0,
                                node_map["raw-input-rate"].u.int64 / 1048576.0);
                        }
                        break;
                    case 10:
                        // 倍速信息
                        if (data) {
                            video_speed = *(double *)data;
                            mpvCoreEvent.fire(VIDEO_SPEED_CHANGE);
                        }
                        break;
                    case 11:
                        // 音量信息
                        if (data) {
                            if (*(int64_t *)data > 0 && volume == 0) {
                                mpvCoreEvent.fire(VIDEO_UNMUTE);
                            } else if (*(int64_t *)data == 0 && volume > 0) {
                                mpvCoreEvent.fire(VIDEO_MUTE);
                            }
                            volume = *(int64_t *)data;
                            mpvCoreEvent.fire(VIDEO_VOLUME_CHANGE);
                        }
                        break;
                    case 12:
                        if (data) video_paused = *(int *)data;
                        if (video_paused) {
                            brls::Logger::info("========> PAUSE");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                        } else if (!video_stopped) {
                            brls::Logger::info("========> RESUME");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                        }
                        break;
                    case 13:
                        if (data) video_stopped = *(int *)data;

                        break;
                    case 14:
                        if (data) video_seeking = *(int *)data;
                        if (video_seeking) {
                            brls::Logger::info("========> VIDEO SEEKING");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        }
                        break;
                    case 15:
                        if (data) {
                            hwCurrent = *(char **)data;
                            {
                                std::lock_guard<std::mutex> lock(videoInfoMutex);
                                videoInfo.hwdec = hwCurrent;
                            }
                            brls::Logger::info("========> HW: {}", hwCurrent);
                            GA("hwdec", {{"hwdec", hwCurrent}})
                        }
                        break;
                    case 16:
                        if (data) filepath = *(char **)data;
                        break;
                    case 17:
                        if (data) video_brightness = *(double *)data;
                        break;
                    case 18:
                        if (data) video_contrast = *(double *)data;
                        break;
                    case 19:
                        if (data) video_saturation = *(double *)data;
                        break;
                    case 20:
                        if (data) video_gamma = *(double *)data;
                        break;
                    case 21:
                        if (data) video_hue = *(double *)data;
                        break;
                    case 22:
                        {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            // 没有视频时 mpv 发来空值，界面据此隐藏这些参数
                            videoInfo.codec = data ? *(char **)data : "";
#if defined(PS5)
                            if (videoInfo.codec.find("ps5") != std::string::npos) {
                                videoInfo.hwdec = "ps5";
                            }
#endif
                        }
                        break;
                    case 23:
                        {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.pixelFormat = data ? *(char **)data : "";
                        }
                        break;
                    case 24:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.width = *(int64_t *)data;
                        }
                        break;
                    case 25:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.height = *(int64_t *)data;
                        }
                        break;
                    case 26:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.fps = *(double *)data;
                        }
                        break;
                    case 27:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.bitrate = *(int64_t *)data;
                        }
                        break;
                    case 28:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.droppedFrames = *(int64_t *)data;
                        }
                        break;
                    case 29:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.avsync = *(double *)data;
                        }
                        break;
                    case 30:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.gamma = *(char **)data ? *(char **)data : "";
                        }
                        break;
                    case 31:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.primaries = *(char **)data ? *(char **)data : "";
                        }
                        break;
                    case 32:
                        if (data) {
                            std::lock_guard<std::mutex> lock(videoInfoMutex);
                            videoInfo.colormatrix = *(char **)data ? *(char **)data : "";
                        }
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
}

void MPVCore::reset() {
    brls::Logger::debug("MPVCore::reset");
    mpvCoreEvent.fire(MpvEventEnum::RESET);
    this->percent_pos    = 0;
    this->duration       = 0;  // second
    this->cache_speed    = 0;  // Bps
    this->playback_time  = 0;
    this->video_progress = 0;
    this->mpv_error_code = 0;
    this->video_aspect   = aspectConverter(MPVCore::VIDEO_ASPECT);

    {
        std::lock_guard<std::mutex> lock(videoInfoMutex);
        videoInfo = {};
    }
    this->currentQuality = 0;

#ifdef MPV_SW_RENDER
    if (pixels && sw_size[0] > 0 && sw_size[1] > 0) {
        memset(pixels, 0, allocated_size);
        this->redraw = true;
    }
    memset(&dovi_meta, 0, sizeof(dovi_meta));
#endif

    // 软硬解切换后应该手动设置一次渲染尺寸
    // 切换视频前设置渲染尺寸可以顺便将上一条视频的最后一帧画面清空
    setFrameSize(rect);
}

void MPVCore::setUrl(const std::string &url, const std::string &extra, const std::string &method) {
    brls::Logger::debug("{} Url: {}, extra: {}", method, url, extra);
#ifdef MPV_SW_RENDER
    if (method == "replace" && pixels && sw_size[0] > 0 && sw_size[1] > 0) {
        memset(pixels, 0, (size_t)sw_size[0] * sw_size[1] * PIXCEL_SIZE);
        this->redraw = true;
    }
    memset(&dovi_meta, 0, sizeof(dovi_meta));
#endif
    if (extra.empty()) {
        command_async("loadfile", url, method);
    } else {
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 3))
            command_async("loadfile", url, method, "0", extra);
        else
            command_async("loadfile", url, method, extra);
    }
}

void MPVCore::setBackupUrl(const std::string &url, const std::string &extra) { this->setUrl(url, extra, "append"); }

void MPVCore::setVolume(int64_t value) {
    if (value < 0 || value > 100) return;
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = (int)value;
}

void MPVCore::setVolume(const std::string &value) {
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = std::stoi(value);
}

int64_t MPVCore::getVolume() const { return this->volume; }

void MPVCore::resume() { command_async("set", "pause", "no"); }

void MPVCore::pause() { command_async("set", "pause", "yes"); }

void MPVCore::stop() {
    command_async("stop");
}

void MPVCore::seek(int64_t p) { command_async("seek", p, "absolute"); }

void MPVCore::seek(const std::string &p) { command_async("seek", p, "absolute"); }

void MPVCore::seekRelative(int64_t p) { command_async("seek", p, "relative"); }

void MPVCore::seekPercent(double p) { command_async("seek", p * 100, "absolute-percent"); }

bool MPVCore::isStopped() const { return video_stopped; }

bool MPVCore::isPlaying() const { return video_playing; }

bool MPVCore::isPaused() const { return video_paused; }

double MPVCore::getSpeed() const { return video_speed; }

void MPVCore::setSpeed(double value) {
    if (video_speed != value) command_async("set", "speed", value);
}

void MPVCore::setAspect(const std::string &value) {
    MPVCore::VIDEO_ASPECT = value;
    video_aspect          = aspectConverter(MPVCore::VIDEO_ASPECT);
    if (value == "-2") {
        // 拉伸全屏
        command_async("set", "keepaspect", "no");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "0.0");
    } else if (value == "-3") {
        // 裁剪填充
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "1.0");
    } else {
        // 指定比例
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", MPVCore::VIDEO_ASPECT);
        command_async("set", "panscan", "0.0");
    }
}

void MPVCore::setMirror(bool value) {
    MPVCore::VIDEO_MIRROR = value;
#ifndef BOREALIS_USE_GXM
    command_async("set", "vf", value ? "hflip" : "");
    setHwdecCopyMode(value);
#endif
}

void MPVCore::setHardwareDecode(bool value) {
    ProgramConfig::instance().setSettingItem(SettingItem::PLAYER_HWDEC, value);
    MPVCore::HARDWARE_DEC = value;
#if defined(PS5)
    std::string vd = value ? PLAYER_HWDEC_METHOD : "!h264_ps5,!hevc_ps5,auto";
    command_async("set", "vd", vd);
    brls::Logger::info("MPV video decoder: {}", vd);
#else
    std::string hwdec = value ? PLAYER_HWDEC_METHOD : "no";
    command_async("set", "hwdec", hwdec);
    brls::Logger::info("MPV hardware decode: {}", hwdec);
#endif
}

void MPVCore::setHwdecCopyMode(bool value) {
#if !defined(PS5)
    // 如果正在使用硬解，那么将硬解更新为 auto-copy，避免直接硬解因为不经过 cpu 处理导致镜像翻转、滤镜无效
    if (MPVCore::HARDWARE_DEC) {
        std::string hwdec = value ? "auto-copy" : MPVCore::PLAYER_HWDEC_METHOD;
        command_async("set", "hwdec", hwdec);
        brls::Logger::info("MPV hardware decode: {}", hwdec);
    }
#endif
}

void MPVCore::setBrightness(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_BRIGHTNESS = value;
    if (video_brightness != value) command_async("set", "brightness", value);
}

void MPVCore::setContrast(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_CONTRAST = value;
    if (video_contrast != value) command_async("set", "contrast", value);
}

void MPVCore::setSaturation(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_SATURATION = value;
    if (video_saturation != value) command_async("set", "saturation", value);
}

void MPVCore::setGamma(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_GAMMA = value;
    if (video_gamma != value) command_async("set", "gamma", value);
}

void MPVCore::setHue(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_HUE = value;
    if (video_hue != value) command_async("set", "hue", value);
}

int MPVCore::getBrightness() const { return video_brightness; }

int MPVCore::getContrast() const { return video_contrast; }

int MPVCore::getSaturation() const { return video_saturation; }

int MPVCore::getGamma() const { return video_gamma; }

int MPVCore::getHue() const { return video_hue; }

// todo: remove these sync function
std::string MPVCore::getString(const std::string &key) {
    char *value = nullptr;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_STRING, &value);
    if (!value) return "";
    std::string result = std::string{value};
    mpvFree(value);
    return result;
}

double MPVCore::getDouble(const std::string &key) {
    double value = 0;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_DOUBLE, &value);
    return value;
}

int64_t MPVCore::getInt(const std::string &key) {
    int64_t value = 0;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_INT64, &value);
    return value;
}

MPVCore::VideoInfo MPVCore::getVideoInfo() {
    std::lock_guard<std::mutex> lock(videoInfoMutex);
    return videoInfo;
}

std::unordered_map<std::string, mpv_node> MPVCore::getNodeMap(const std::string &key) {
    mpv_node node;
    std::unordered_map<std::string, mpv_node> nodeMap;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_NODE, &node) < 0) return nodeMap;
    if (node.format != MPV_FORMAT_NODE_MAP) return nodeMap;
    // todo: 目前不要使用 mpv_node中有指针的部分，因为这些内容指向的内存会在这个函数结束的时候删除
    for (int i = 0; i < node.u.list->num; i++) {
        char *nodeKey = node.u.list->keys[i];
        if (nodeKey == nullptr) continue;
        nodeMap.insert(std::make_pair(std::string{nodeKey}, node.u.list->values[i]));
    }
    mpvFreeNodeContents(&node);
    return nodeMap;
}

double MPVCore::getPlaybackTime() const { return playback_time; }

void MPVCore::disableDimming(bool disable) {
    brls::Logger::info("disableDimming: {}", disable);
    brls::Application::getPlatform()->disableScreenDimming(disable, "Playing video", APPVersion::getPackageName());
    static bool deactivationAvailable = ProgramConfig::instance().getSettingItem(SettingItem::DEACTIVATED_TIME, 0) > 0;
    if (deactivationAvailable) {
        brls::Application::setAutomaticDeactivation(!disable);
    }
}

void MPVCore::setShader(const std::string &profile, const std::string &shaders,
                        const std::vector<std::vector<std::string>> &settings, bool reset) {
    brls::Logger::info("Set shader [{}]: {}", profile, shaders);

    // 如果之前设置的shader包含mpv配置，就需要重置一下
    if (!currentSetting.empty() && reset) clearShader(false);

    currentShaderProfile = profile;
    currentShader        = shaders;
    currentSetting       = settings;

    // 设置着色器
    if (!shaders.empty()) command_async("no-osd", "change-list", "glsl-shaders", "set", shaders);

    // 设置mpv配置
    for (auto &setting : settings) {
        _command_async(setting);
    }

    // 显示通知
    if (reset) brls::Application::notify(profile);
}

void MPVCore::clearShader(bool showHint) {
    brls::Logger::info("Clear shader");

    // 如果当前不涉及mpv配置修改，就无需重置
    bool reset = !currentSetting.empty();

    currentShader.clear();
    currentShaderProfile.clear();
    currentSetting.clear();

    // 清空着色器
    command_async("no-osd", "change-list", "glsl-shaders", "clr", "");

    // 重置mpv配置
    if (reset) MPVCore::instance().restart();

    // 显示通知
    if (showHint) brls::Application::notify("Clear profile");
}

void MPVCore::showOsdText(const std::string &value, int d) { command_async("show-text", value, d); }

void MPVCore::_command_async(const std::vector<std::string> &commands) {
    std::vector<const char *> res;
    res.reserve(commands.size() + 1);
    for (auto &i : commands) {
        res.emplace_back(i.c_str());
    }
    res.emplace_back(nullptr);
    mpvCommandAsync(mpv, 0, res.data());
}