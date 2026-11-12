#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/memory/Memory.h"
#include "cordite/rhi/DeviceBackend.h"

// The Direct3D 12 of DirectX-Headers, before the DXGI of the Windows SDK: both declare what
// they share behind the same include guards, and the newer declarations have to win.
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <fmt/format.h>
#include <nvrhi/d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

using Microsoft::WRL::ComPtr;

/// The least of Direct3D 12 the engine draws with: what nearly every GPU of the last decade
/// and WARP have.
constexpr D3D_FEATURE_LEVEL kMinimumFeatureLevel = D3D_FEATURE_LEVEL_11_0;
/// The shader model the engine's shaders are compiled for (ShaderTarget::Dxil).
constexpr D3D_SHADER_MODEL kMinimumShaderModel = D3D_SHADER_MODEL_6_0;

std::string_view featureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
        case D3D_FEATURE_LEVEL_12_2:
            return "12_2";
        case D3D_FEATURE_LEVEL_12_1:
            return "12_1";
        case D3D_FEATURE_LEVEL_12_0:
            return "12_0";
        case D3D_FEATURE_LEVEL_11_1:
            return "11_1";
        case D3D_FEATURE_LEVEL_11_0:
            return "11_0";
        default:
            return "unknown";
    }
}

/// "6.6" for D3D_SHADER_MODEL_6_6, whose number is 0x66.
std::string shaderModelName(D3D_SHADER_MODEL model) {
    const auto number = static_cast<unsigned>(model);
    return fmt::format("{}.{}", number >> 4u, number & 0xFu);
}

/// The text of a wide string of Windows, in UTF-8.
std::string utf8Of(const wchar_t* text) {
    return pathToUtf8(std::filesystem::path(std::wstring_view(text)));
}

/// The newest shader model the device runs; nothing for one older than 6.0. The runtime
/// refuses to be asked about a model newer than itself, so it is asked from the newest the
/// headers know down.
std::optional<D3D_SHADER_MODEL> highestShaderModel(ID3D12Device* device) {
    for (int model = D3D_HIGHEST_SHADER_MODEL; model >= D3D_SHADER_MODEL_6_0; --model) {
        D3D12_FEATURE_DATA_SHADER_MODEL data{static_cast<D3D_SHADER_MODEL>(model)};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
            return data.HighestShaderModel;
        }
    }
    return std::nullopt;
}

D3D_FEATURE_LEVEL highestFeatureLevel(ID3D12Device* device) {
    constexpr std::array<D3D_FEATURE_LEVEL, 5> kLevels = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                                          D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                                          D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
    data.NumFeatureLevels = static_cast<UINT>(kLevels.size());
    data.pFeatureLevelsRequested = kLevels.data();
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data)))) {
        return kMinimumFeatureLevel;
    }
    return data.MaxSupportedFeatureLevel;
}

/// An adapter that passed every requirement, what it is, and how much it is wanted.
struct AdapterCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    AdapterInfo info;
    int score = 0;
};

/// Checks one adapter against what the engine needs. `software` for WARP.
Result<AdapterCandidate> evaluateAdapter(const ComPtr<IDXGIAdapter1>& adapter, bool software,
                                         std::string_view preferredAdapter) {
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description))) {
        return makeError(ErrorCode::DeviceError, "cannot be described");
    }
    AdapterCandidate candidate{.adapter = adapter};
    AdapterInfo& info = candidate.info;
    info.name = utf8Of(description.Description);

    // A device of an adapter is one object, whoever asks for it: this is the one that
    // will draw, should the adapter be chosen.
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), kMinimumFeatureLevel, IID_PPV_ARGS(&device)))) {
        return makeError(ErrorCode::Unsupported, "does not run Direct3D 12 at feature level {}",
                         featureLevelName(kMinimumFeatureLevel));
    }
    const std::optional<D3D_SHADER_MODEL> model = highestShaderModel(device.Get());
    if (!model || *model < kMinimumShaderModel) {
        return makeError(ErrorCode::Unsupported, "runs shaders of a model older than {}",
                         shaderModelName(kMinimumShaderModel));
    }

    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    const bool unified =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture))) &&
        architecture.UMA != FALSE;
    if (software) {
        info.type = AdapterType::Software;
    } else {
        info.type = unified ? AdapterType::Integrated : AdapterType::Discrete;
    }
    info.vendorId = description.VendorId;
    info.deviceId = description.DeviceId;
    info.dedicatedMemoryBytes = description.DedicatedVideoMemory;
    info.apiVersion = fmt::format("{}, shader model {}", featureLevelName(highestFeatureLevel(device.Get())),
                                  shaderModelName(*model));
    // Every queue that draws can be timed, and every device can draw wireframes.
    info.supportsTimerQueries = true;
    info.supportsWireframe = true;
    candidate.score = adapterScore(info.type, info.name, preferredAdapter);
    return candidate;
}

/// What a message of the debug layer is to the device: an issue, from a warning up.
void recordMessage(ValidationSink& issues, D3D12_MESSAGE_SEVERITY severity, std::string_view text) {
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            CORDITE_LOG_ERROR("Direct3D", "{}", text);
            issues.recordIssue(fmt::format("Direct3D 12 error: {}", text));
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            CORDITE_LOG_WARN("Direct3D", "{}", text);
            issues.recordIssue(fmt::format("Direct3D 12 warning: {}", text));
            break;
        default:
            CORDITE_LOG_DEBUG("Direct3D", "{}", text);
            break;
    }
}

/// What GetDeviceRemovedReason says, by the name DXGI gives it.
std::string_view removedReasonName(HRESULT reason) {
    switch (reason) {
        case DXGI_ERROR_DEVICE_HUNG:
            return "the device hung";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "the device was removed";
        case DXGI_ERROR_DEVICE_RESET:
            return "the device was reset";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return "the driver failed";
        case DXGI_ERROR_INVALID_CALL:
            return "a call was invalid";
        default:
            return "another reason";
    }
}

/// The objects of Direct3D 12 behind a GraphicsDevice.
struct D3D12Backend final : public DeviceBackend {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    /// Where the debug layer keeps what it says, while it runs. It is collected from there
    /// rather than called back with: every version of the layer keeps them, only newer
    /// ones call back, and one way of hearing them is one way to be tested.
    ComPtr<ID3D12InfoQueue> infoQueue;
    /// Told what the debug layer reports; outlives the backend.
    ValidationSink* issues = nullptr;

    explicit D3D12Backend(ValidationSink& sink) : issues(&sink) {}

    CORDITE_NON_MOVABLE(D3D12Backend);
    ~D3D12Backend() override = default;

    void collectMessages() override {
        if (!infoQueue) {
            return;
        }
        const UINT64 count = infoQueue->GetNumStoredMessages();
        // A message is its header and its text, in one block of the size it says.
        std::vector<std::byte> storage;
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T length = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &length)) || length < sizeof(D3D12_MESSAGE)) {
                continue;
            }
            storage.resize(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(index, message, &length))) {
                recordMessage(*issues, message->Severity,
                              message->pDescription != nullptr ? message->pDescription : "");
            }
        }
        infoQueue->ClearStoredMessages();
    }

    [[nodiscard]] std::optional<std::string> lostReason() const override {
        if (!device) {
            return std::nullopt;
        }
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (SUCCEEDED(reason)) {
            return std::nullopt;
        }
        return fmt::format("Direct3D 12 removed the device: {} (0x{:08X})", removedReasonName(reason),
                           static_cast<uint32_t>(reason));
    }
};

/// The adapter to draw with: of those that run what the engine needs, the one most wanted
/// - hardware by the preference of the system for performance, and WARP last.
Result<AdapterCandidate> selectAdapter(IDXGIFactory6* factory, std::string_view preferredAdapter) {
    std::optional<AdapterCandidate> best;
    std::string rejections;
    const auto consider = [&](const ComPtr<IDXGIAdapter1>& adapter, bool software) {
        Result<AdapterCandidate> candidate = evaluateAdapter(adapter, software, preferredAdapter);
        if (!candidate) {
            DXGI_ADAPTER_DESC1 description{};
            const std::string name = SUCCEEDED(adapter->GetDesc1(&description)) ? utf8Of(description.Description)
                                                                                : std::string("an adapter");
            rejections += fmt::format("{}{}: {}", rejections.empty() ? "" : "; ", name, candidate.error().message);
            return;
        }
        if (!best || candidate->score > best->score) {
            best = std::move(*candidate);
        }
    };

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 description{};
        // The software adapter in the list is WARP, which is asked for by name below.
        if (adapter && SUCCEEDED(adapter->GetDesc1(&description)) &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            consider(adapter, false);
        }
        adapter = nullptr;
    }
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        consider(adapter, true);
    }

    if (!best) {
        return makeError(ErrorCode::Unsupported, "no usable graphics adapter was found ({})",
                         rejections.empty() ? "the system reports none for Direct3D 12" : rejections);
    }
    return std::move(*best);
}

} // namespace

Result<BackendDevice> createD3D12Device(const GraphicsDeviceDesc& desc, nvrhi::IMessageCallback& messages,
                                        ValidationSink& issues) {
    auto backend = std::make_unique<D3D12Backend>(issues);

    // The debug layer is a part of Windows that has to be installed ("Graphics Tools"), and
    // is switched on before there is a device, or not at all.
    bool debugLayer = false;
    if (desc.validation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debugLayer = true;
        } else {
            CORDITE_LOG_WARN("RHI", "the debug layer of Direct3D 12 is not installed; only NVRHI validation is "
                                    "active. Add Windows' optional feature \"Graphics Tools\" for full validation.");
        }
    }

    if (FAILED(CreateDXGIFactory2(debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&backend->factory)))) {
        return makeError(ErrorCode::Unsupported, "DXGI 1.6, which Direct3D 12 needs, is not there; update Windows");
    }
    CORDITE_TRY_ASSIGN(AdapterCandidate chosen, selectAdapter(backend->factory.Get(), desc.preferredAdapter));

    if (FAILED(D3D12CreateDevice(chosen.adapter.Get(), kMinimumFeatureLevel, IID_PPV_ARGS(&backend->device)))) {
        return makeError(ErrorCode::DeviceError, "Direct3D 12 could not make a device of {}", chosen.info.name);
    }
    if (debugLayer && FAILED(backend->device.As(&backend->infoQueue))) {
        CORDITE_LOG_WARN("RHI", "the debug layer of Direct3D 12 runs, but what it says cannot be heard");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(backend->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&backend->queue)))) {
        return makeError(ErrorCode::DeviceError, "Direct3D 12 could not make a queue of {}", chosen.info.name);
    }
    backend->queue->SetName(L"Cordite graphics queue");

    nvrhi::d3d12::DeviceDesc deviceDesc;
    deviceDesc.errorCB = &messages;
    deviceDesc.pDevice = backend->device.Get();
    deviceDesc.pGraphicsCommandQueue = backend->queue.Get();
    BackendDevice made;
    made.device = nvrhi::d3d12::createDevice(deviceDesc);
    if (!made.device) {
        return makeError(ErrorCode::DeviceError, "NVRHI could not create its Direct3D 12 device");
    }

    made.adapter = std::move(chosen.info);
    // Validation that nobody hears is none.
    made.apiValidation = backend->infoQueue != nullptr;
    CORDITE_LOG_INFO("RHI", "Using {} ({}, Direct3D 12 at {}, {} MiB){}", made.adapter.name, nameOf(made.adapter.type),
                     made.adapter.apiVersion, made.adapter.dedicatedMemoryBytes / kMebibyte,
                     made.apiValidation ? ", validation on" : "");
    // A window is drawn into with Vulkan alone, so far (Swapchain::create).
    made.canPresent = false;
    made.backend = std::move(backend);
    return made;
}

} // namespace cordite
