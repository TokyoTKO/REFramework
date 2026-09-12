#include <openvr.h>
#include <chrono>
#include <fstream>
#include <MinHook.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <../../directxtk12-src/Inc/ScreenGrab.h>
#include <utility/ScopeGuard.hpp>

#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>
#include <../../directxtk12-src/Src/d3dx12.h>

#include "D3D12Component.hpp"

#if defined(PRAGMATA)
namespace pragmata_resolve_capture {
using Microsoft::WRL::ComPtr;
struct Context {
    ID3D12GraphicsCommandList* cmd{};
    ID3D12PipelineState* target{};
    ID3D12PipelineState* current{};
    ID3D12Resource* output[2]{};
    ComPtr<ID3D12Resource> before[2], after[2];
    D3D12_RESOURCE_STATES state[2]{};
    bool known[2]{};
    bool captured{};
    unsigned int capture_mask{};
    unsigned int matches{};
};
static thread_local Context* active = nullptr;
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PipelineFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
static DispatchFn original_dispatch{};
static PipelineFn original_pipeline{};
static BarrierFn original_barrier{};

static void STDMETHODCALLTYPE pipeline_hook(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso) {
    if (active && active->cmd == cmd) active->current = pso;
    original_pipeline(cmd, pso);
}
static void STDMETHODCALLTYPE barrier_hook(ID3D12GraphicsCommandList* cmd, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (active && active->cmd == cmd) {
        for (UINT j = 0; j < count; ++j) {
            const auto& b = barriers[j];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            for (int i = 0; i < 2; ++i) {
                if (b.Transition.pResource != active->output[i]) continue;
                if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                    (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0)) {
                    active->known[i] = false;
                } else {
                    active->state[i] = b.Transition.StateAfter;
                    active->known[i] = true;
                }
            }
        }
    }
    original_barrier(cmd, count, barriers);
}
static void copy_outputs(Context& c, ComPtr<ID3D12Resource>* destinations, unsigned int mask) {
    for (int i = 0; i < 2; ++i) {
        if (!(mask & (1u << i))) continue;
        const auto state = c.state[i];
        // A state transition orders prior UAV writes before the copy.
        // If already COPY_SOURCE, an explicit UAV barrier is unnecessary.
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(c.output[i], state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            original_barrier(c.cmd, 1, &b);
        }
        c.cmd->CopyResource(destinations[i].Get(), c.output[i]);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(c.output[i], D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            original_barrier(c.cmd, 1, &b);
        }
    }
}
static void STDMETHODCALLTYPE dispatch_hook(ID3D12GraphicsCommandList* cmd, UINT x, UINT y, UINT z) {
    auto* c = active;
    const bool target = c && c->cmd == cmd && c->current == c->target;
    if (target) ++c->matches;
    const unsigned int mask = target && !c->captured ?
        (c->known[0] ? 1u : 0u) | (c->known[1] ? 2u : 0u) : 0u;
    const bool capture = mask != 0;
    if (target) spdlog::info("[PragmataResolveV8] at dispatch known={},{} states={},{} mask={}",
        c->known[0], c->known[1], static_cast<unsigned int>(c->state[0]), static_cast<unsigned int>(c->state[1]), mask);
    if (capture) copy_outputs(*c, c->before, mask);
    original_dispatch(cmd, x, y, z);
    if (capture) {
        copy_outputs(*c, c->after, mask);
        c->capture_mask = mask;
        c->captured = true;
        spdlog::info("[PragmataResolveV8] captured resolve dispatch={}x{}x{} states={},{}", x, y, z,
            static_cast<unsigned int>(c->state[0]), static_cast<unsigned int>(c->state[1]));
    }
}

static bool install(ID3D12GraphicsCommandList* cmd) {
    static bool attempted = false;
    static bool installed = false;
    if (attempted) return installed;
    attempted = true;
    auto** vtable = *reinterpret_cast<void***>(cmd);
    void* targets[]{vtable[14], vtable[25], vtable[26]};
    void* hooks[]{reinterpret_cast<void*>(&dispatch_hook), reinterpret_cast<void*>(&pipeline_hook), reinterpret_cast<void*>(&barrier_hook)};
    void** originals[]{reinterpret_cast<void**>(&original_dispatch), reinterpret_cast<void**>(&original_pipeline), reinterpret_cast<void**>(&original_barrier)};
    unsigned int created = 0;
    for (; created < 3; ++created) {
        const auto result = MH_CreateHook(targets[created], hooks[created], originals[created]);
        if (result != MH_OK) {
            spdlog::error("[PragmataResolveV8] hook creation failed index={} status={}; stage capture disabled", created, static_cast<int>(result));
            for (unsigned int i = 0; i < created; ++i) MH_RemoveHook(targets[i]);
            return false;
        }
    }
    for (unsigned int i = 0; i < 3; ++i) {
        const auto result = MH_QueueEnableHook(targets[i]);
        if (result != MH_OK) {
            for (unsigned int j = 0; j < 3; ++j) MH_RemoveHook(targets[j]);
            spdlog::error("[PragmataResolveV8] hook queue failed; stage capture disabled");
            return false;
        }
    }
    if (MH_ApplyQueued() != MH_OK) {
        for (unsigned int i = 0; i < 3; ++i) { MH_DisableHook(targets[i]); MH_RemoveHook(targets[i]); }
        spdlog::error("[PragmataResolveV8] hook enable failed; stage capture disabled");
        return false;
    }
    installed = true;
    spdlog::info("[PragmataResolveV8] command-list hooks installed; inactive outside requested AFW capture");
    return true;
}
static ID3D12PipelineState* resolve_pso() {
    static HMODULE verified_module = nullptr;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        auto module = GetModuleHandleW(L"PDAFWPlugin.dll");
        wchar_t path[32768]{};
        const auto length = module ? GetModuleFileNameW(module, path, 32768) : 0;
        if (!length || length >= 32768) return nullptr;
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        uint64_t hash = 14695981039346656037ull;
        size_t size = 0;
        char ch;
        while (file.get(ch)) { hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ull; ++size; }
        // Exact supplied beta6 file fingerprint, plus exported entrypoint validation.
        if (!file.eof() || size != 534528 || hash != 0x094c5e71fccd8f6bull ||
            GetProcAddress(module, "EvaluateFrameWarp") != reinterpret_cast<FARPROC>(reinterpret_cast<uintptr_t>(module) + 0x19ab0)) {
            spdlog::error("[PragmataResolveV8] plugin fingerprint mismatch; stage capture disabled");
            return nullptr;
        }
        verified_module = module;
        spdlog::info("[PragmataResolveV8] beta6 plugin fingerprint verified");
    }
    if (!verified_module) return nullptr;
    uintptr_t object = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(verified_module) + 0x99cf0), &object, sizeof(object), &read) || read != sizeof(object) || !object) return nullptr;
    ID3D12PipelineState* pso = nullptr;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + 0x5c8), &pso, sizeof(pso), &read) || read != sizeof(pso)) return nullptr;
    return pso;
}
static bool prepare(Context& c, ID3D12Device* device, ID3D12GraphicsCommandList* cmd, EyeFrameBuffers& buffers) {
    c.target = resolve_pso();
    if (!c.target || !install(cmd)) return false;
    c.cmd = cmd;
    for (int i = 0; i < 2; ++i) {
        c.output[i] = buffers.eyeFrameBuffers[i].color.pTexture;
        if (!c.output[i]) return false;
        // The plugin supplies each texture's resting state in its API descriptor.
        // Use that explicit contract until an observed transition updates it.
        c.state[i] = buffers.eyeFrameBuffers[i].color.initialState;
        c.known[i] = c.state[i] == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        spdlog::info("[PragmataResolveV8] eye={} descriptor state={} seeded={}",
            i, static_cast<unsigned int>(c.state[i]), c.known[i]);
        auto desc = c.output[i]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) return false;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(c.before[i].GetAddressOf()))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(c.after[i].GetAddressOf())))) return false;
    }
    return true;
}
struct Scope {
    Context* previous;
    explicit Scope(Context* c) : previous(active) { active = c; }
    ~Scope() { active = previous; }
};
}
#endif

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
#if defined(PRAGMATA)
    // Observation only: no GPU readback, waits, rendering changes or hotkeys.
    using StartupClock = std::chrono::steady_clock;
    static const auto startup_begin = StartupClock::now();
    static auto startup_previous = startup_begin;
    static auto startup_window = startup_begin;
    static double startup_sum_ms = 0.0;
    static double startup_max_ms = 0.0;
    static unsigned int startup_samples = 0;
    const auto startup_now = StartupClock::now();
    const double startup_elapsed = std::chrono::duration<double>(startup_now - startup_begin).count();
    const double startup_dt = std::chrono::duration<double, std::milli>(startup_now - startup_previous).count();
    startup_previous = startup_now;
    const bool startup_active = startup_elapsed < 120.0;
    bool startup_report = false;
    if (startup_active) {
        startup_sum_ms += startup_dt;
        if (startup_dt > startup_max_ms) startup_max_ms = startup_dt;
        ++startup_samples;
        if (std::chrono::duration<double>(startup_now - startup_window).count() >= 0.25) {
            startup_report = true;
            spdlog::info("[PragmataStartupV5] t={:.3f} samples={} submitMeanMs={:.3f} submitMaxMs={:.3f} frame={} afw={} uiFix={} foveated={} depth={} reset={}",
                startup_elapsed, startup_samples, startup_sum_ms / startup_samples, startup_max_ms,
                vr->m_render_frame_count, vr->is_using_any_afw(), vr->m_enable_ui_fix->value(),
                vr->is_foveated_rendering(), vr->depthTex[0] != nullptr, m_force_reset);
            startup_samples = 0;
            startup_sum_ms = startup_max_ms = 0.0;
            startup_window = startup_now;
        }
    }
#endif

    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }
    auto eye_texture = backbuffer;

    if (!m_backbuffer_is_8bit && false) {
        auto command_list = m_backbuffer_copy.commands.cmd_list.Get();
        m_backbuffer_copy.commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        m_backbuffer_copy.commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        m_backbuffer_copy.commands.clear_rtv(m_converted_eye_tex, clear_color, D3D12_RESOURCE_STATE_PRESENT);

        // Convert the backbuffer to 8-bit.
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_backbuffer_copy.commands.execute();

        eye_texture = m_converted_eye_tex.texture;
    }

    auto runtime = vr->get_runtime();
#if defined(PRAGMATA)
    static bool capture_key_down = false;
    static unsigned int capture_pending = 0;
    static ULONGLONG capture_requested = 0;
    static std::filesystem::path capture_folder;
    const bool capture_key_now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (capture_key_now && !capture_key_down && capture_pending == 0) {
        capture_requested = GetTickCount64();
        capture_pending = 3;
        spdlog::info("[PragmataAFWCaptureV4] armed; capture in 3 seconds; no AFW parameters changed");
    }
    capture_key_down = capture_key_now;
    if (capture_pending && GetTickCount64() - capture_requested > 15000) {
        capture_pending = 0;
        spdlog::warn("[PragmataAFWCaptureV4] capture expired before both eyes were saved");
    }
#endif


    const auto frame_count = vr->m_render_frame_count;

    
    //#############################
    //#Frame Warp Module Start
    //#############################
    EyeIndex nEye = (frame_count % 2 == vr->m_left_eye_interval) ? EyeLeft : EyeRight;
    EyeIndex nEyeOther = (frame_count % 2 == vr->m_left_eye_interval) ? EyeRight : EyeLeft;
    auto eyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEye];
    auto otherEyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEyeOther];
    FrameWarpEvaluateParams params;
    if ((vr->is_using_any_afw()) && (!m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture || !m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture))
        force_reset();

    auto colorDesc = eyeFrameBuffer.color.pTexture->GetDesc();
    static TextureDesc texDesc[4];
    int texIndex = m_backbuffer_is_8bit || true ? backbuffer_index : 3;
    if (texDesc[texIndex].pTexture != eye_texture.Get()) {
        texDesc[texIndex].pTexture = eye_texture.Get();
        texDesc[texIndex].initialState = D3D12_RESOURCE_STATE_PRESENT;
        if (texIndex == 3)
            texDesc[texIndex].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        vr->d3d12Renderer->SetupTextureDesc(texDesc[texIndex]);
    }
    if (texDesc[backbuffer_index].pTexture != backbuffer.Get()) {
        texDesc[backbuffer_index].pTexture = backbuffer.Get();
        texDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(texDesc[backbuffer_index]);
    }
    
    if (vr->m_enable_ui_fix->value() ) {
        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture != vr->uiBufferTex[i]) {
                vr->uiBufferDesc[i].pTexture = vr->uiBufferTex[i];
                vr->uiBufferDesc[i].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                vr->d3d12Renderer->SetupTextureDesc(vr->uiBufferDesc[i]);
            }
        }
        if (vr->is_using_afw_foveated() && vr->uiBufferTex[0]) {
            auto desc = vr->uiBufferTex[0]->GetDesc();
            if (vr->multipassUIBufferDesc.pTexture == NULL || vr->multipassUIBufferDesc.pTexture->GetDesc().Width != colorDesc.Width ||
                vr->multipassUIBufferDesc.pTexture->GetDesc().Height != colorDesc.Height || vr->multipassUIBufferDesc.pTexture->GetDesc().Format != desc.Format) {
                vr->d3d12Renderer->CreateTexture(colorDesc.Width, colorDesc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->multipassUIBufferDesc, false);
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        if (vr->depthDesc[i].pTexture != vr->depthTex[i]) {
            vr->depthDesc[i].pTexture = vr->depthTex[i];
            vr->depthDesc[i].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            vr->d3d12Renderer->SetupTextureDesc(vr->depthDesc[i]);
            vr->depthDesc[i].type = Depth;
        }
    }
    if (vr->depthTex[0]) {
        auto desc = vr->depthTex[0]->GetDesc();
        if (vr->motionVectorsDesc.pTexture == NULL || vr->motionVectorsDesc.pTexture->GetDesc().Width != desc.Width ||
            vr->motionVectorsDesc.pTexture->GetDesc().Height != desc.Height) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsDesc, true);
        }
        if (vr->is_using_afw_foveated() &&
            (vr->motionVectorsCorrectedDesc.pTexture == NULL || vr->motionVectorsCorrectedDesc.pTexture->GetDesc().Width != desc.Width ||
                vr->motionVectorsCorrectedDesc.pTexture->GetDesc().Height != desc.Height)) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsCorrectedDesc, true);
        }
        if (vr->is_using_afw_foveated() &&
            (vr->foveatedDepthDesc.pTexture == NULL || vr->foveatedDepthDesc.pTexture->GetDesc().Width != desc.Width ||
                vr->foveatedDepthDesc.pTexture->GetDesc().Height != desc.Height)) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->foveatedDepthDesc, false);
        }
    }

#if defined(PRAGMATA)
    bool capture_this_frame = false;
    ComPtr<ID3D12Resource> capture_scene;
    ComPtr<ID3D12Resource> capture_ui;
    const unsigned int capture_bit = 1u << static_cast<unsigned int>(nEye);
    if ((capture_pending & capture_bit) && GetTickCount64() - capture_requested >= 3000 &&
        vr->is_using_any_afw() && !vr->is_foveated_rendering() && vr->depthTex[0] &&
        vr->uiBufferDesc[0].pTexture) {
        capture_pending &= ~capture_bit;
        try {
            if (capture_pending == (3u & ~capture_bit)) {
                wchar_t exe_path[32768]{};
                const auto length = GetModuleFileNameW(nullptr, exe_path, 32768);
                if (!length || length >= 32768) throw std::runtime_error("Cannot resolve game directory");
                capture_folder = std::filesystem::path(exe_path).parent_path() /
                    L"reframework" / L"data" / (L"afw_resolve_v8_capture_" + std::to_wstring(capture_requested));
                std::filesystem::create_directories(capture_folder);
                spdlog::info("[PragmataAFWCaptureV4] folder={}", capture_folder.string());
            }
            const auto allocate_snapshot = [&](ID3D12Resource* src, ComPtr<ID3D12Resource>& dst) {
                auto desc = src->GetDesc();
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(dst.GetAddressOf()));
            };
            const auto scene_hr = allocate_snapshot(texDesc[texIndex].pTexture, capture_scene);
            const auto ui_hr = allocate_snapshot(vr->uiBufferDesc[0].pTexture, capture_ui);
            capture_this_frame = SUCCEEDED(scene_hr) && SUCCEEDED(ui_hr);
            if (!capture_this_frame) {
                capture_pending = 0;
                spdlog::error("[PragmataAFWCaptureV4] snapshot allocation failed scene={} ui={}",
                    static_cast<long>(scene_hr), static_cast<long>(ui_hr));
            }
        } catch (const std::exception& e) {
            capture_pending = 0;
            spdlog::error("[PragmataAFWCaptureV4] capture failed: {}", e.what());
        }
    }
#endif

    auto cmdList = vr->d3d12Renderer->BeginCommandList(backbuffer_index);
#if defined(PRAGMATA)
    pragmata_resolve_capture::Context resolve_capture;
    const bool resolve_ready = capture_this_frame && pragmata_resolve_capture::prepare(resolve_capture, device, cmdList, m_eyeFrameBuffers);
    if (capture_this_frame && !resolve_ready) spdlog::warn("[PragmataResolveV8] stage capture unavailable; ordinary capture retained");
#endif
#if defined(PRAGMATA)
    if (capture_this_frame) {
        // GPU snapshots are queued before AFW, with no CPU readback wait between input and output.
        const auto snapshot = [&](ID3D12Resource* src, D3D12_RESOURCE_STATES state, ID3D12Resource* dst) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmdList->ResourceBarrier(1, &barrier);
            cmdList->CopyResource(dst, src);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            cmdList->ResourceBarrier(1, &barrier);
        };
        snapshot(texDesc[texIndex].pTexture, texDesc[texIndex].initialState, capture_scene.Get());
        snapshot(vr->uiBufferDesc[0].pTexture, vr->uiBufferDesc[0].initialState, capture_ui.Get());
    }
#endif


    if ((vr->is_using_any_afw()) && m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture && vr->depthTex[0]) {
        static FrameBufferDesc s_CurrentEyeFrameBuffer{};

        s_CurrentEyeFrameBuffer.color = texDesc[texIndex];
        s_CurrentEyeFrameBuffer.depth = vr->depthDesc[0];
        s_CurrentEyeFrameBuffer.motionVectors = vr->motionVectorsDesc;

	    if (!vr->get_runtime()->hiddenAreaMeshVextexBuffer[0].pVextexBuffer && vr->get_runtime()->hiddenAreaMesh[0].pVertexData) {
            UINT vertexSize = sizeof(vr::HmdVector2_t);
            vr->d3d12Renderer->CreateVertexBuffer(cmdList, vr->get_runtime()->hiddenAreaMeshVextexBuffer[0], vr->get_runtime()->hiddenAreaMesh[0].unTriangleCount * 3, vertexSize,
                (float*)vr->get_runtime()->hiddenAreaMesh[0].pVertexData);
            vr->d3d12Renderer->CreateVertexBuffer(cmdList, vr->get_runtime()->hiddenAreaMeshVextexBuffer[1], vr->get_runtime()->hiddenAreaMesh[1].unTriangleCount * 3, vertexSize,
                (float*)vr->get_runtime()->hiddenAreaMesh[1].pVertexData);
        }

        params.InCmdList = cmdList;
        params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
        params.InUIColorAlpha = NULL;
        params.IsHudlessColor = true;
        params.MotionVectorsType = vr->is_fix_dlss()? Normal : FromOtherEye;
        params.InMotionScale[0] = (float)colorDesc.Width;
        params.InMotionScale[1] = (float)colorDesc.Height;
        params.Mode = (FrameWarpMode)vr->m_framewarp_mode->value();
        params.EyeIndex = nEye;
        params.ClearBeforeWarping = vr->m_clear_before_framewarp->value();
        params.CameraData = &vr->cameraData[nEye];
        params.IgnoreMotionThreshold = vr->m_ignore_motion_threshold->value();
        params.Debug = vr->m_framewarp_debug->value();

        if (!vr->is_foveated_rendering()) {
            if (vr->m_enable_ui_fix->value() && vr->uiBufferDesc[0].pTexture) {
                params.InUIColorAlpha = &vr->uiBufferDesc[0];
                params.IsHudlessColor = false;
            }
            // Sharpening
            if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
                vr->d3d12Renderer->Sharpen(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, vr->get_sharpness());
                s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
            }

#if defined(PRAGMATA)
            // Emit on state transitions as well as the periodic timing sample.
            static unsigned int startup_last_state = ~0u;
            const unsigned int startup_state = (unsigned int)params.Mode |
                ((unsigned int)params.MotionVectorsType << 4) |
                ((params.InUIColorAlpha != nullptr ? 1u : 0u) << 8) |
                ((params.IsHudlessColor ? 1u : 0u) << 9) |
                ((params.ClearBeforeWarping ? 1u : 0u) << 10);
            if (startup_active && (startup_report || startup_state != startup_last_state)) {
                spdlog::info("[PragmataStartupV5] t={:.3f} evaluate eye={} mode={} mvType={} ui={} hudless={} clear={} width={} height={} dlssFix={}",
                    startup_elapsed, (int)nEye, (int)params.Mode, (int)params.MotionVectorsType,
                    params.InUIColorAlpha != nullptr, params.IsHudlessColor, params.ClearBeforeWarping,
                    colorDesc.Width, colorDesc.Height, vr->is_fix_dlss());
                startup_last_state = startup_state;
            }
#endif
            {
#if defined(PRAGMATA)
                pragmata_resolve_capture::Scope scope(resolve_ready ? &resolve_capture : nullptr);
#endif
                EvaluateFrameWarp(params);
            }

        } else if (vr->is_using_afw_foveated()) {
            auto foveatedVP = vr->get_runtime()->foveated_viewports[nEye];
            D3D12_VIEWPORT vp;
            vp.TopLeftX = foveatedVP.TopLeftX * colorDesc.Width;
            vp.TopLeftY = foveatedVP.TopLeftY * colorDesc.Height;
            vp.Width = foveatedVP.Width * colorDesc.Width;
            vp.Height = foveatedVP.Height * colorDesc.Height;
            vp.MinDepth = 0;
            vp.MaxDepth = 1;
            FLOAT black[4] = {0, 0, 0, 0};
            if (vr->multipassBackupDesc[nEye].pTexture) {
                float edgeCutoutRatio = vr->get_edge_scan_line_fix_range();
                float blurRadius = vr->get_foveated_periphery_blur();
                edgeCutoutRatio = 1.0f - 1.0f / (1.0f + edgeCutoutRatio);
                auto desc = s_CurrentEyeFrameBuffer.color.pTexture->GetDesc();
                D3D12_BOX srcBox = (nEye == EyeRight) ? CD3DX12_BOX(edgeCutoutRatio * desc.Width, 0, desc.Width, desc.Height)
                                                      : CD3DX12_BOX(0, 0, (1 - edgeCutoutRatio) * desc.Width, desc.Height);
                if (vr->mDebug1 || (edgeCutoutRatio <= 0 && blurRadius <= 0)) {
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color);
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1]);
                }else if (edgeCutoutRatio > 0 && blurRadius <= 0) {
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, srcBox);
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1], srcBox);
                } else if (edgeCutoutRatio > 0 && blurRadius > 0) {
                    vr->d3d12Renderer->Crop(cmdList, otherEyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, srcBox);
                    vr->d3d12Renderer->Blur(cmdList, eyeFrameBuffer.color, otherEyeFrameBuffer.color, blurRadius);
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1], srcBox);
                } else {
                    vr->d3d12Renderer->Blur(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, blurRadius);
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1]);
                }
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->foveatedDepthDesc, vp);

                if (vr->m_enable_ui_fix->value() && vr->multipassUIBufferDesc.pTexture) {
                    if (edgeCutoutRatio > 0) {
                        vr->d3d12Renderer->Crop(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1], srcBox);
                    } else {
                        vr->d3d12Renderer->Blit(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1]);
                    }
                    params.InUIColorAlpha = &vr->multipassUIBufferDesc;
                    params.IsHudlessColor = false;
                }
                TonemapParams params;
                params.fGamma = 1.10f;
                params.fLowerLimit = 0.024f;
                params.fUpperLimit = 0.982f;
                params.fConvertToLimit = 0.958f;
                vr->d3d12Renderer->Tonemap(cmdList, vr->multipassBackupDesc[nEye], vr->uiBufferDesc[0], params);
                vr->d3d12Renderer->FoveatedComposite(cmdList, eyeFrameBuffer.color, vr->multipassBackupDesc[nEye], vp, vr->get_foveated_composite_params());
            } else {
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color);
                if (vr->m_enable_ui_fix->value() && vr->multipassUIBufferDesc.pTexture) {
                    vr->d3d12Renderer->Blit(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1]);
                    params.InUIColorAlpha = &vr->multipassUIBufferDesc;
                    params.IsHudlessColor = false;
                }
            }


            vr->d3d12Renderer->Clear(cmdList, eyeFrameBuffer.motionVectors, black);
            if (vr->motionVectorsDesc.pTexture && vr->motionVectorsCorrectedDesc.pTexture) {
                CorrectMotionVectorsParams mvparams;
                mvparams.inMotionVectors = &vr->motionVectorsDesc;
                mvparams.inDepth = &vr->foveatedDepthDesc;
                mvparams.cameraData = &vr->cameraDataForMV[nEye];
                mvparams.InMotionScale[0] = (float)vr->motionVectorsDesc.pTexture->GetDesc().Width;
                mvparams.InMotionScale[1] = (float)vr->motionVectorsDesc.pTexture->GetDesc().Height;
                mvparams.extractObjectOnlyMotion = true;
                vr->d3d12Renderer->CorrectMotionVectors(cmdList, vr->motionVectorsCorrectedDesc, mvparams);
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.motionVectors, vr->motionVectorsCorrectedDesc, vp);
            }

            // Sharpening
            if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
                vr->d3d12Renderer->Sharpen(cmdList, otherEyeFrameBuffer.color, eyeFrameBuffer.color, vr->get_sharpness());
                vr->d3d12Renderer->Copy(cmdList, eyeFrameBuffer.color, otherEyeFrameBuffer.color);
            }
            if (vr->m_desktop_fix->value()) {
                vr->d3d12Renderer->Blit(cmdList, texDesc[texIndex], eyeFrameBuffer.color);
            }
            s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
            s_CurrentEyeFrameBuffer.depth = eyeFrameBuffer.depth;
            s_CurrentEyeFrameBuffer.motionVectors = eyeFrameBuffer.motionVectors;
            params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
            params.MotionVectorsType = ObjectOnly;
            params.isFoveated = true;
            auto foveatedVPOther = vr->get_runtime()->foveated_viewports[nEyeOther];
            D3D12_VIEWPORT vpOther;
            vpOther.TopLeftX = foveatedVPOther.TopLeftX * colorDesc.Width;
            vpOther.TopLeftY = foveatedVPOther.TopLeftY * colorDesc.Height;
            vpOther.Width = foveatedVPOther.Width * colorDesc.Width;
            vpOther.Height = foveatedVPOther.Height * colorDesc.Height;
            params.foveatedArea = RECT(vpOther.TopLeftX, vpOther.TopLeftY, vpOther.TopLeftX + vpOther.Width, vpOther.TopLeftY + vpOther.Height);

#if defined(PRAGMATA)
            // Emit on state transitions as well as the periodic timing sample.
            static unsigned int startup_last_state = ~0u;
            const unsigned int startup_state = (unsigned int)params.Mode |
                ((unsigned int)params.MotionVectorsType << 4) |
                ((params.InUIColorAlpha != nullptr ? 1u : 0u) << 8) |
                ((params.IsHudlessColor ? 1u : 0u) << 9) |
                ((params.ClearBeforeWarping ? 1u : 0u) << 10);
            if (startup_active && (startup_report || startup_state != startup_last_state)) {
                spdlog::info("[PragmataStartupV5] t={:.3f} evaluate eye={} mode={} mvType={} ui={} hudless={} clear={} width={} height={} dlssFix={}",
                    startup_elapsed, (int)nEye, (int)params.Mode, (int)params.MotionVectorsType,
                    params.InUIColorAlpha != nullptr, params.IsHudlessColor, params.ClearBeforeWarping,
                    colorDesc.Width, colorDesc.Height, vr->is_fix_dlss());
                startup_last_state = startup_state;
            }
#endif
            {
#if defined(PRAGMATA)
                pragmata_resolve_capture::Scope scope(resolve_ready ? &resolve_capture : nullptr);
#endif
                EvaluateFrameWarp(params);
            }
        }

        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture) {
                D3D12_RESOURCE_BARRIER barriers1[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, vr->uiBufferDesc[i].initialState, D3D12_RESOURCE_STATE_RENDER_TARGET)};
                FLOAT black[4] = {0, 0, 0, 0};
                cmdList->ResourceBarrier(_countof(barriers1), barriers1);

                cmdList->ClearRenderTargetView(vr->uiBufferDesc[i].renderTargetViewHandle, black, 0, NULL);

                D3D12_RESOURCE_BARRIER barriers2[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, vr->uiBufferDesc[i].initialState)};
                cmdList->ResourceBarrier(_countof(barriers2), barriers2);
            }
        }
    } else {
        vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, texDesc[texIndex]);
        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture) {
                D3D12_RESOURCE_BARRIER barriers1[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, vr->uiBufferDesc[i].initialState, D3D12_RESOURCE_STATE_RENDER_TARGET)};
                FLOAT black[4] = {0, 0, 0, 0};
                cmdList->ResourceBarrier(_countof(barriers1), barriers1);

                cmdList->ClearRenderTargetView(vr->uiBufferDesc[i].renderTargetViewHandle, black, 0, NULL);

                D3D12_RESOURCE_BARRIER barriers2[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, vr->uiBufferDesc[i].initialState)};
                cmdList->ResourceBarrier(_countof(barriers2), barriers2);
            }
        }
    }

    vr->d3d12Renderer->EndCommandList(backbuffer_index);
#if defined(PRAGMATA)
    if (capture_this_frame) {
        try {
            const auto origin = nEye == EyeLeft ? L"from_left_" : L"from_right_";
            const auto save = [&](ID3D12Resource* tex, D3D12_RESOURCE_STATES state, const wchar_t* name) {
                if (!tex) return;
                const auto file = capture_folder / (std::wstring(origin) + name + L".dds");
                const auto desc = tex->GetDesc();
                const auto hr = DirectX::SaveDDSTextureToFile(command_queue, tex, file.c_str(), state, state);
                spdlog::info("[PragmataAFWCaptureV4] file={} inputEye={} frame={} texture={} size={}x{} format={} state={} hr={}",
                    file.string(), static_cast<int>(nEye), frame_count, static_cast<void*>(tex),
                    desc.Width, desc.Height, static_cast<int>(desc.Format), static_cast<unsigned int>(state),
                    static_cast<long>(hr));
            };
            // Output states match the existing OpenXR submission copies below.
            save(m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"output_left");
            save(m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"output_right");
            if (resolve_capture.captured) {
                if (resolve_capture.capture_mask & 1u) {
                    save(resolve_capture.before[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"before_resolve_left");
                    save(resolve_capture.after[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"after_resolve_left");
                }
                if (resolve_capture.capture_mask & 2u) {
                    save(resolve_capture.before[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"before_resolve_right");
                    save(resolve_capture.after[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"after_resolve_right");
                }
            }
            spdlog::info("[PragmataResolveV8] eye={} ready={} matches={} captured={} known={},{}",
                static_cast<int>(nEye), resolve_ready, resolve_capture.matches, resolve_capture.captured,
                resolve_capture.known[0], resolve_capture.known[1]);
            save(capture_scene.Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"input_scene");
            save(capture_ui.Get(), D3D12_RESOURCE_STATE_COPY_DEST, L"input_ui");
            spdlog::info("[PragmataAFWCaptureV4] inputEye={} uiFix={} isHudless={} mode={} capturePending={}",
                static_cast<int>(nEye), vr->m_enable_ui_fix->value(), params.IsHudlessColor,
                vr->m_framewarp_mode->value(), capture_pending);
            if (!capture_pending) spdlog::info("[PragmataAFWCaptureV4] finished; check file HRESULTs");
        } catch (const std::exception& e) {
            capture_pending = 0;
            spdlog::error("[PragmataAFWCaptureV4] output capture failed: {}", e.what());
        }
    }
#endif


    //#############################
    //#Frame Warp Module End
    //#############################

    //bool submitFrame = (vr->is_using_sf() || vr->is_using_any_afw());
    //if (!submitFrame) {
    //    hook->ignore_next_present();
    //    return vr::EVRCompositorError::VRCompositorError_None;
    //}

    auto eye_format = backbuffer->GetDesc().Format;

    if (runtime->is_openxr()) {
        if (eye_format != m_openxr.last_format) {
            spdlog::info("[VR] OpenXR format changed from {} to {}", m_openxr.last_format, eye_format);
            m_openxr.create_swapchains();
        }
    } else {
        if (eye_format != m_openvr.last_format) {
            spdlog::info("[VR] OpenVR format changed from {} to {}", m_openvr.last_format, eye_format);
            on_reset(vr);
            setup();
        }
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (frame_count % 2 == vr->m_left_eye_interval) {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(0, m_openvr.get_left().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            if (vr->is_using_any_afw()) {
                m_openxr.copy(1, m_openvr.get_right().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            //m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
            if (vr->is_using_any_afw()) {
                //auto& ctx = m_openvr.acquire_right();
                //if (params.OutEyeColor && vr->m_use_reprojection->value()) {
                //    ctx.commands.copy((ID3D12Resource*)params.OutEyeColor, ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                //}
                //ctx.commands.execute();

                vr::D3D12TextureData_t right{m_openvr.get_right().texture.Get(), command_queue, 0};

                vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

                auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                    return e;
                } else {
                    vr->m_submitted = true;
                }

                ++m_openvr.texture_counter;
            }
        }
    } else {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(1, m_openvr.get_right().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            if (vr->is_using_any_afw()) {
                m_openxr.copy(0, m_openvr.get_left().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            //m_openvr.copy_right(eye_texture.Get());

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }
            if (vr->is_using_any_afw()) {
                //auto& ctx = m_openvr.acquire_left();
                //if (params.OutEyeColor && vr->m_use_reprojection->value()) {
                //    ctx.commands.copy((ID3D12Resource*)params.OutEyeColor, ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                //}
                //ctx.commands.execute();

                vr::D3D12TextureData_t left{m_openvr.get_left().texture.Get(), command_queue, 0};

                vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    return e;
                }
            }
            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (frame_count % 2 == vr->m_right_eye_interval || vr->is_using_any_afw()) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        if (vr->m_desktop_fix->value() && frame_count % 2 == vr->m_right_eye_interval) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::on_reset(VR* vr) {
    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::setup() {

    if (VR::get()->is_hmd_active()) {
        spdlog::info("[VR] Setting up d3d12 textures...");
    }
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};

    const auto& vr = VR::get();

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    if (backbuffer == nullptr) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    backbuffer_desc.Width = real_backbuffer_desc.Width;
    backbuffer_desc.Height = real_backbuffer_desc.Height;

    m_openvr.last_format = backbuffer_desc.Format;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, backbuffer_desc.Format);
    spdlog::info("[VR] D3D12 Real Backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, real_backbuffer_desc.Format);

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || backbuffer_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    auto rt_desc = backbuffer_desc;

    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        
        default:
            spdlog::error("[OpenVR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };

    //#############################
    //#Frame Warp Module Start
    //#############################
    static uint32_t lastSize[2]{0, 0};
    static DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
    if ((lastSize[0] != vr->get_hmd_width() || lastSize[1] != vr->get_hmd_height() || lastFormat != rt_desc.Format)) {
        FrameWarpInitParams params = {vr->get_hmd_width(), vr->get_hmd_height(), rt_desc.Format};
        m_eyeFrameBuffers = InitFrameWarp(params);
        lastSize[0] = vr->get_hmd_width();
        lastSize[1] = vr->get_hmd_height();
    }
    //#############################
    //#Frame Warp Module End
    //#############################

    // Create converted eye texture
    if (!m_backbuffer_is_8bit && false) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture != NULL) {
            left_eye_tex = m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture;
        } else {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return;
            }
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture != NULL) {
            right_eye_tex = m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture;
        } else {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return;
            }
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = real_backbuffer_desc.Width;
    m_backbuffer_size[1] = real_backbuffer_desc.Height;

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    const auto dst_desc = dst.texture->GetDesc();
    const auto src_desc = src.texture->GetDesc();
    
    auto& batch = m_sprite_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dst_desc.Width;
    scissor_rect.bottom = (LONG)dst_desc.Height;

    // Transition dst to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst.texture.Get();

    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { dst.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { src.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(src.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)src_desc.Width, (uint32_t)src_desc.Height },
        dest_rect,
        DirectX::Colors::White);

    batch->End();

    // Transition dst to dst_state
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = dst_state;
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    this->contexts.resize(openxr->views.size());

    this->last_format = backbuffer_desc.Format;

    DXGI_FORMAT swapchain_format{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            swapchain_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        
        default:
            spdlog::error("[VR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (swapchain_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else {
            backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        spdlog::info("[VR] Format: {}", backbuffer_desc.Format);

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = swapchain_format;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear();

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }

        ctx.textures.clear();
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    D3D12_BOX* src_box, 
    D3D12_RESOURCE_STATES src_state)
{

    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);
            if (src_box != nullptr) {
                texture_ctx->commands.copy_region(
                    resource, 
                    ctx.textures[texture_index].texture, 
                    src_box, src_state, 
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            } else {
                texture_ctx->commands.copy(
                    resource, 
                    ctx.textures[texture_index].texture, 
                    src_state, 
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
        }
    }
}
} // namespace vrmod


