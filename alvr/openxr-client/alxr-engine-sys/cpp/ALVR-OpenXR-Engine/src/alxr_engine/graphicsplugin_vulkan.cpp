// Copyright (c) 2017-2022, The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "logger.h"
#include "pch.h"
#include "common.h"
#include "geometry.h"
#include "options.h"
#include "graphicsplugin.h"

#ifdef XR_USE_GRAPHICS_API_VULKAN

#if !defined(NDEBUG)
    #define XR_ENABLE_VULKAN_VALIDATION_LAYER
#endif

#include <common/xr_linear.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>

#ifdef USE_ONLINE_VULKAN_SHADERC
#include <shaderc/shaderc.hpp>
#endif

#ifdef XR_USE_PLATFORM_ANDROID
#include <media/NdkImage.h>
#endif

#include "cuda/WindowsSecurityAttributes.h"
#ifdef XR_ENABLE_CUDA_INTEROP
#include "cuda/vulkancuda_interop.h"
#endif

#if defined(VK_USE_PLATFORM_WIN32_KHR)
// Define USE_MIRROR_WINDOW to open a otherwise-unused window for e.g. RenderDoc
//#define USE_MIRROR_WINDOW
#endif

// glslangValidator doesn't wrap its output in brackets if you don't have it define the whole array.
#if defined(USE_GLSLANGVALIDATOR)
#define SPV_PREFIX {
#define SPV_SUFFIX }
#else
#define SPV_PREFIX
#define SPV_SUFFIX
#endif

#if defined(XR_USE_GRAPHICS_API_D3D11)
#include "d3d_common.h"
#endif

#include <readerwritercircularbuffer.h>
#include "concurrent_queue.h"
#include "timing.h"
#include "foveation.h"

namespace {

inline std::string vkResultString(VkResult res) {
    switch (res) {
        case VK_SUCCESS:
            return "SUCCESS";
        case VK_NOT_READY:
            return "NOT_READY";
        case VK_TIMEOUT:
            return "TIMEOUT";
        case VK_EVENT_SET:
            return "EVENT_SET";
        case VK_EVENT_RESET:
            return "EVENT_RESET";
        case VK_INCOMPLETE:
            return "INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:
            return "SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
            return "ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:
            return "ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV:
            return "ERROR_INVALID_SHADER_NV";
        default:
            return std::to_string(res);
    }
}

[[noreturn]] inline void ThrowVkResult(VkResult res, const char* originator = nullptr, const char* sourceLocation = nullptr) {
    Throw(Fmt("VkResult failure [%s]", vkResultString(res).c_str()), originator, sourceLocation);
}

inline VkResult CheckVkResult(VkResult res, const char* originator = nullptr, const char* sourceLocation = nullptr) {
    if ((res) < VK_SUCCESS) {
        ThrowVkResult(res, originator, sourceLocation);
    }

    return res;
}

// XXX These really shouldn't have trailing ';'s
#define THROW_VK(res, cmd) ThrowVkResult(res, #cmd, FILE_AND_LINE);
#define CHECK_VKCMD(cmd) CheckVkResult(cmd, #cmd, FILE_AND_LINE);
#define CHECK_VKRESULT(res, cmdStr) CheckVkResult(res, cmdStr, FILE_AND_LINE);

#ifdef USE_ONLINE_VULKAN_SHADERC
constexpr char VertexShaderGlsl[] =
    R"_(
    #version 430
    #extension GL_ARB_separate_shader_objects : enable

    layout (std140, push_constant) uniform buf
    {
        mat4 mvp;
    } ubuf;

    layout (location = 0) in vec3 Position;
    layout (location = 1) in vec3 Color;

    layout (location = 0) out vec4 oColor;
    out gl_PerVertex
    {
        vec4 gl_Position;
    };

    void main()
    {
        oColor.rgba  = Color.rgba;
        gl_Position = ubuf.mvp * Position;
    }
)_";

constexpr char FragmentShaderGlsl[] =
    R"_(
    #version 430
    #extension GL_ARB_separate_shader_objects : enable

    layout (location = 0) in vec4 oColor;

    layout (location = 0) out vec4 FragColor;

    void main()
    {
        FragColor = oColor;
    }
)_";
#endif  // USE_ONLINE_VULKAN_SHADERC

struct MemoryAllocator {
    void Init(VkPhysicalDevice physicalDevice, VkDevice device) {
        m_physicalDevice = physicalDevice;
        m_vkDevice = device;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &m_memProps);
    }

    static constexpr const VkFlags defaultFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    void Allocate(VkMemoryRequirements const& memReqs, VkDeviceMemory* mem, VkFlags flags = defaultFlags,
                  const void* pNext = nullptr) const {
        // Search memtypes to find first index with those properties
        for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
            if ((memReqs.memoryTypeBits & (1 << i)) != 0u) {
                // Type is available, does it match user properties?
                if ((m_memProps.memoryTypes[i].propertyFlags & flags) == flags) {
                    const VkMemoryAllocateInfo memAlloc {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                        .pNext = pNext,
                        .allocationSize = memReqs.size,
                        .memoryTypeIndex = i,
                    };
                    CHECK_VKCMD(vkAllocateMemory(m_vkDevice, &memAlloc, nullptr, mem));
                    return;
                }
            }
        }
        THROW("Memory format not supported");
    }

    std::uint32_t FindMemoryType
    (
        const std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    ) const
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

        for (std::uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
                return i;
            }
        }
        throw std::runtime_error("failed to find suitable memory type!");
    }

   private:
    VkPhysicalDevice m_physicalDevice{ VK_NULL_HANDLE };
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties m_memProps{};
};

struct SemaphoreTimeline {
    VkDevice                   device{ VK_NULL_HANDLE };
    VkSemaphore                fence{ VK_NULL_HANDLE };
    std::atomic<std::uint64_t> fenceValue{ 0 };
    //HANDLE                     fenceEvent = INVALID_HANDLE_VALUE;

    inline SemaphoreTimeline() noexcept = default;
    inline SemaphoreTimeline(SemaphoreTimeline&&) noexcept = delete;
    inline SemaphoreTimeline& operator=(SemaphoreTimeline&&) noexcept = delete;

    inline SemaphoreTimeline(const SemaphoreTimeline&) noexcept = delete;
    inline SemaphoreTimeline& operator=(const SemaphoreTimeline&) noexcept = delete;

    inline ~SemaphoreTimeline() {
        if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE)
            vkDestroySemaphore(device, fence, nullptr);
    }

    static constexpr inline VkExternalSemaphoreHandleTypeFlagBits GetDefaultHandleType() {
#ifdef _WIN64
        return true//IsWindows8OrGreater()
            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
            : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
#else
        return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif /* _WIN64 */
    }

    void Create(VkDevice vkdevice, const bool makeExternal = false)
    {
        device = vkdevice;
        assert(device != VK_NULL_HANDLE);
        const VkSemaphoreTypeCreateInfo timelineCreateInfo {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = fenceValue.load()
        };
        VkSemaphoreCreateInfo createInfo {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineCreateInfo,
            .flags = 0
        };
        if (makeExternal)
        {
#ifdef _WIN64
            const WindowsSecurityAttributes winSecurityAttributes{};
            const VkExportSemaphoreWin32HandleInfoKHR vulkanExportSemaphoreWin32HandleInfoKHR {
                .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .pAttributes = &winSecurityAttributes,
                .dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                .name = (LPCWSTR)nullptr
            };
#endif
            const VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo {
                .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR,
#ifdef _WIN64
                .pNext = /*IsWindows8OrGreater()*/true ? &vulkanExportSemaphoreWin32HandleInfoKHR : nullptr,
                .handleTypes = /*IsWindows8OrGreater()*/ true ?
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT :
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
#else
                .pNext = nullptr,
                .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            };
            createInfo.pNext = &vulkanExportSemaphoreCreateInfo;
            CHECK_VKCMD(vkCreateSemaphore(device, &createInfo, nullptr, &fence));
        }
        else
            CHECK_VKCMD(vkCreateSemaphore(device, &createInfo, nullptr, &fence));
    }

    void SignalExec(VkQueue queue, VkSubmitInfo& submitInfo, VkFence execFence = VK_NULL_HANDLE)
    {
        const std::uint64_t fenceVal = fenceValue.load() + 1;
        fenceValue.store(fenceVal);
        const VkTimelineSemaphoreSubmitInfo timelineInfo1 {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreValueCount = 0,
            .pWaitSemaphoreValues = nullptr,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &fenceVal
        };
        submitInfo.pNext = &timelineInfo1;
        submitInfo.waitSemaphoreCount = 0;
        submitInfo.pWaitSemaphores = nullptr;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &fence;

        CHECK_VKCMD(vkQueueSubmit(queue, 1, &submitInfo, execFence));
    }

    template < const std::size_t Size >
    using SemaArray = std::array<SemaphoreTimeline*, Size>;

    template < const std::size_t Size >
    using WaiterPipelineFlags = std::array<VkPipelineStageFlags, Size>;

    template < const std::size_t SignalCount, const std::size_t WaiterCount >
    struct SubmitInfo
    {
        template < typename Tp >
        using WaiterArray = std::array<Tp, WaiterCount>;
        template < typename Tp >
        using SignalerArray = std::array<Tp, SignalCount>;

        SignalerArray<std::uint64_t>    signalValues;
        WaiterArray<std::uint64_t>      waiterValues;
        SignalerArray<VkSemaphore>      signalers;
        WaiterArray<VkSemaphore>        waiters;
        VkTimelineSemaphoreSubmitInfo   timelineInfo{};
        VkSubmitInfo                    submitInfo {};

        constexpr inline SubmitInfo(const SubmitInfo&) = default;
        constexpr inline SubmitInfo& operator=(const SubmitInfo&) = default;

        inline SubmitInfo
        (
            const SemaArray<SignalCount>& signalersp,
            const SemaArray<WaiterCount>& waitersp,
            const WaiterPipelineFlags<WaiterCount>& waiterPipelineFlags
        )
        {
            for (std::size_t index = 0; index < waitersp.size(); ++index)
            {
                SemaphoreTimeline& waiter = *waitersp[index];
                waiterValues[index] = waiter.fenceValue.load();
                this->waiters[index] = waiter.fence;
            }

            for (std::size_t index = 0; index < signalersp.size(); ++index)
            {
                auto& signaler = *signalersp[index];
                const std::uint64_t fenceVal = signaler.fenceValue.load() + 1;
                signaler.fenceValue.store(fenceVal);
                signalValues[index] = fenceVal;
                this->signalers[index] = signaler.fence;
            }

            timelineInfo = {
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .waitSemaphoreValueCount = static_cast<std::uint32_t>(waiterValues.size()),
                .pWaitSemaphoreValues = waiterValues.data(),
                .signalSemaphoreValueCount = static_cast<std::uint32_t>(signalValues.size()),
                .pSignalSemaphoreValues = signalValues.data()
            };

            submitInfo = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timelineInfo,
                .waitSemaphoreCount = static_cast<std::uint32_t>(waiters.size()),
                .pWaitSemaphores = waiters.data(),
                .pWaitDstStageMask = waiterPipelineFlags.data(),
                .signalSemaphoreCount = static_cast<std::uint32_t>(signalers.size()),
                .pSignalSemaphores = signalers.data()
            };
        }
    };

    template < const std::size_t SignalCount, const std::size_t WaiterCount >
    static inline SubmitInfo<SignalCount, WaiterCount> make_submit_info
    (
        const SemaArray<SignalCount>& signalers,
        const SemaArray<WaiterCount>& waiters,
        const WaiterPipelineFlags<WaiterCount>& waiterPipelineFlags
    )
    {
        return { signalers, waiters, waiterPipelineFlags };
    }
};

// CmdBuffer - manage VkCommandBuffer state
struct CmdBuffer {
#define LIST_CMDBUFFER_STATES(_) \
    _(Undefined)                 \
    _(Initialized)               \
    _(Recording)                 \
    _(Executable)                \
    _(Executing)
    enum class CmdBufferState {
#define MK_ENUM(name) name,
        LIST_CMDBUFFER_STATES(MK_ENUM)
#undef MK_ENUM
    };
    CmdBufferState state{CmdBufferState::Undefined};
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer buf{VK_NULL_HANDLE};
    VkFence execFence{VK_NULL_HANDLE};

    CmdBuffer() = default;

    CmdBuffer(const CmdBuffer&) = delete;
    CmdBuffer& operator=(const CmdBuffer&) = delete;
    CmdBuffer(CmdBuffer&&) = delete;
    CmdBuffer& operator=(CmdBuffer&&) = delete;

    ~CmdBuffer() {
        SetState(CmdBufferState::Undefined);
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (buf != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(m_vkDevice, pool, 1, &buf);
            }
            if (pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(m_vkDevice, pool, nullptr);
            }
            if (execFence != VK_NULL_HANDLE) {
                vkDestroyFence(m_vkDevice, execFence, nullptr);
            }
        }
        buf = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        execFence = VK_NULL_HANDLE;
        m_vkDevice = nullptr;
    }

    std::string StateString(CmdBufferState s) {
        switch (s) {
#define MK_CASE(name)          \
    case CmdBufferState::name: \
        return #name;
            LIST_CMDBUFFER_STATES(MK_CASE)
#undef MK_CASE
        }
        return "(Unknown)";
    }

#define CHECK_CBSTATE(s)                                                                                           \
    do                                                                                                             \
        if (state != (s)) {                                                                                        \
            Log::Write(Log::Level::Error,                                                                          \
                       std::string("Expecting state " #s " from ") + __FUNCTION__ + ", in " + StateString(state)); \
            return false;                                                                                          \
        }                                                                                                          \
    while (0)

    bool Init(VkDevice device, uint32_t queueFamilyIndex) {
        CHECK_CBSTATE(CmdBufferState::Undefined);

        m_vkDevice = device;
        assert(m_vkDevice != VK_NULL_HANDLE);
        // Create a command pool to allocate our command buffer from
        const VkCommandPoolCreateInfo cmdPoolInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamilyIndex
        };
        CHECK_VKCMD(vkCreateCommandPool(m_vkDevice, &cmdPoolInfo, nullptr, &pool));

        // Create the command buffer from the command pool
        const VkCommandBufferAllocateInfo cmd {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        CHECK_VKCMD(vkAllocateCommandBuffers(m_vkDevice, &cmd, &buf));

        constexpr const VkFenceCreateInfo fenceInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0
        };
        CHECK_VKCMD(vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &execFence));

        SetState(CmdBufferState::Initialized);
        return true;
    }

    bool Begin() {
        CHECK_CBSTATE(CmdBufferState::Initialized);
        constexpr const VkCommandBufferBeginInfo cmdBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = 0
        };
        CHECK_VKCMD(vkBeginCommandBuffer(buf, &cmdBeginInfo));
        SetState(CmdBufferState::Recording);
        return true;
    }

    bool End() {
        CHECK_CBSTATE(CmdBufferState::Recording);
        CHECK_VKCMD(vkEndCommandBuffer(buf));
        SetState(CmdBufferState::Executable);
        return true;
    }

    bool Exec(VkQueue queue)
    {
        CHECK_CBSTATE(CmdBufferState::Executable);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &buf
        };
        CHECK_VKCMD(vkQueueSubmit(queue, 1, &submitInfo, execFence));

        SetState(CmdBufferState::Executing);
        return true;
    }

    template < const std::size_t Size >
    using SemaArray = std::array<SemaphoreTimeline*, Size>;

    template < const std::size_t Size >
    using WaiterPipelineFlags = std::array<VkPipelineStageFlags, Size>;

    template < const std::size_t SignalCount, const std::size_t WaiterCount >
    bool Exec
    (
        VkQueue queue,
        const SemaArray<SignalCount>& signalers,
        const SemaArray<WaiterCount>& waiters,
        const WaiterPipelineFlags<WaiterCount>& waiterPipelineFlags
    )
    {
        CHECK_CBSTATE(CmdBufferState::Executable);

        auto submitInfo = SemaphoreTimeline::make_submit_info(signalers, waiters, waiterPipelineFlags);
        submitInfo.submitInfo.commandBufferCount = 1;
        submitInfo.submitInfo.pCommandBuffers = &buf;

        CHECK_VKCMD(vkQueueSubmit(queue, 1, &submitInfo.submitInfo, execFence));

        SetState(CmdBufferState::Executing);
        return true;
    }

    template < const std::size_t WaiterCount >
    bool ExecWaiters
    (
        VkQueue queue,
        const SemaArray<WaiterCount>& waiters,
        const WaiterPipelineFlags<WaiterCount>& waiterPipelineFlags
    )
    {
        return Exec<0, WaiterCount>(queue, {}, waiters, waiterPipelineFlags);
    }

    template < const std::size_t SignalerCount >
    bool ExecSignalers
    (
        VkQueue queue,
        const SemaArray<SignalerCount>& signalers
    )
    {
        return Exec<SignalerCount, 0>(queue, signalers, {}, {});
    }

    bool Wait() {
        // Waiting on a not-in-flight command buffer is a no-op
        if (state == CmdBufferState::Initialized) {
            return true;
        }

        CHECK_CBSTATE(CmdBufferState::Executing);

        const uint32_t timeoutNs = 1 * 1000 * 1000 * 1000;
        for (int i = 0; i < 5; ++i) {
            auto res = vkWaitForFences(m_vkDevice, 1, &execFence, VK_TRUE, timeoutNs);
            if (res == VK_SUCCESS) {
                // Buffer can be executed multiple times...
                SetState(CmdBufferState::Executable);
                return true;
            }
            Log::Write(Log::Level::Info, "Waiting for CmdBuffer fence timed out, retrying...");
        }

        return false;
    }

    bool Reset() {
        if (state != CmdBufferState::Initialized) {
            CHECK_CBSTATE(CmdBufferState::Executable);

            CHECK_VKCMD(vkResetFences(m_vkDevice, 1, &execFence));
            CHECK_VKCMD(vkResetCommandBuffer(buf, 0));

            SetState(CmdBufferState::Initialized);
        }

        return true;
    }

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};

    void SetState(CmdBufferState newState) { state = newState; }

#undef CHECK_CBSTATE
#undef LIST_CMDBUFFER_STATES
};

// ShaderProgram to hold a pair of vertex & fragment shaders
struct ShaderProgram {
    std::array<VkPipelineShaderStageCreateInfo, 2> shaderInfo{{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .pSpecializationInfo = nullptr
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .pSpecializationInfo = nullptr
        }
    }};

    ShaderProgram() = default;
    ~ShaderProgram() {
        if (m_vkDevice != nullptr) {
            for (auto& si : shaderInfo) {
                if (si.module != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(m_vkDevice, shaderInfo[0].module, nullptr);
                }
                si.module = VK_NULL_HANDLE;
            }
        }
        shaderInfo = {};
        m_vkDevice = nullptr;
    }

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&&) = delete;
    ShaderProgram& operator=(ShaderProgram&&) = delete;


    constexpr static const char* const EntryPoint = "main";
    using CodeBuffer = std::vector<std::uint32_t>;

    void LoadVertexShader(const CodeBuffer& code,   const char* const mainName=EntryPoint) { Load(0, code, mainName); }
    void LoadFragmentShader(const CodeBuffer& code, const char* const mainName=EntryPoint) { Load(1, code, mainName); }
    
    void Init(VkDevice device) { m_vkDevice = device; }

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};

    void Load(uint32_t index, const CodeBuffer& code, const char* const mainName) {

        auto& si = shaderInfo[index];
        si.pName = mainName;
        std::string name;
        switch (index) {
            case 0:
                si.stage = VK_SHADER_STAGE_VERTEX_BIT;
                name = "vertex";
                break;
            case 1:
                si.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                name = "fragment";
                break;
            default:
                THROW(Fmt("Unknown code index %d", index));
        }

        const VkShaderModuleCreateInfo modInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = code.size() * sizeof(code[0]),
            .pCode = &code[0],
        };
        CHECK_MSG((modInfo.codeSize > 0) && modInfo.pCode, Fmt("Invalid %s shader ", name.c_str()));

        CHECK_VKCMD(vkCreateShaderModule(m_vkDevice, &modInfo, nullptr, &si.module));

        Log::Write(Log::Level::Info, Fmt("Loaded %s shader", name.c_str()));
    }
};

// VertexBuffer base class
struct VertexBufferBase {
    VkBuffer idxBuf{VK_NULL_HANDLE};
    VkDeviceMemory idxMem{VK_NULL_HANDLE};
    VkBuffer vtxBuf{VK_NULL_HANDLE};
    VkDeviceMemory vtxMem{VK_NULL_HANDLE};
    VkVertexInputBindingDescription bindDesc{};
    std::vector<VkVertexInputAttributeDescription> attrDesc{};
    struct {
        uint32_t idx;
        uint32_t vtx;
    } count = {0, 0};

    VertexBufferBase() = default;

    ~VertexBufferBase() {
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (idxBuf != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_vkDevice, idxBuf, nullptr);
            }
            if (idxMem != VK_NULL_HANDLE) {
                vkFreeMemory(m_vkDevice, idxMem, nullptr);
            }
            if (vtxBuf != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_vkDevice, vtxBuf, nullptr);
            }
            if (vtxMem != VK_NULL_HANDLE) {
                vkFreeMemory(m_vkDevice, vtxMem, nullptr);
            }
        }
        idxBuf = VK_NULL_HANDLE;
        idxMem = VK_NULL_HANDLE;
        vtxBuf = VK_NULL_HANDLE;
        vtxMem = VK_NULL_HANDLE;
        bindDesc = {};
        attrDesc.clear();
        count = {0, 0};
        m_vkDevice = VK_NULL_HANDLE;
    }

    VertexBufferBase(const VertexBufferBase&) = delete;
    VertexBufferBase& operator=(const VertexBufferBase&) = delete;
    VertexBufferBase(VertexBufferBase&&) = delete;
    VertexBufferBase& operator=(VertexBufferBase&&) = delete;
    void Init(VkDevice device, const MemoryAllocator* memAllocator, const std::vector<VkVertexInputAttributeDescription>& attr) {
        m_vkDevice = device;
        m_memAllocator = memAllocator;
        attrDesc = attr;
    }

   protected:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    void AllocateBufferMemory(VkBuffer buf, VkDeviceMemory* mem) const {
        VkMemoryRequirements memReq = {};
        vkGetBufferMemoryRequirements(m_vkDevice, buf, &memReq);
        m_memAllocator->Allocate(memReq, mem);
    }

   private:
    const MemoryAllocator* m_memAllocator{nullptr};
};

// VertexBuffer template to wrap the indices and vertices
template <typename T>
struct VertexBuffer final : public VertexBufferBase {
    bool Create(uint32_t idxCount, uint32_t vtxCount) {
        VkBufferCreateInfo bufInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .size = sizeof(uint16_t) * idxCount,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT            
        };
        CHECK_VKCMD(vkCreateBuffer(m_vkDevice, &bufInfo, nullptr, &idxBuf));
        AllocateBufferMemory(idxBuf, &idxMem);
        CHECK_VKCMD(vkBindBufferMemory(m_vkDevice, idxBuf, idxMem, 0));

        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufInfo.size = sizeof(T) * vtxCount;
        CHECK_VKCMD(vkCreateBuffer(m_vkDevice, &bufInfo, nullptr, &vtxBuf));
        AllocateBufferMemory(vtxBuf, &vtxMem);
        CHECK_VKCMD(vkBindBufferMemory(m_vkDevice, vtxBuf, vtxMem, 0));

        bindDesc = {
            .binding = 0,
            .stride = sizeof(T),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
        };

        count = {idxCount, vtxCount};

        return true;
    }

    inline void UpdateIndices(const std::uint16_t* data, const std::uint32_t size, const std::uint32_t offset = 0) {
        uint16_t* map = nullptr;
        CHECK_VKCMD(vkMapMemory(m_vkDevice, idxMem, sizeof(map[0]) * offset, sizeof(map[0]) * size, 0, (void**)&map));
        std::copy_n(data, size, map);
        vkUnmapMemory(m_vkDevice, idxMem);
    }

    inline void UpdateVertices(const T* data, const std::uint32_t size, const std::uint32_t offset = 0) {
        T* map = nullptr;
        CHECK_VKCMD(vkMapMemory(m_vkDevice, vtxMem, sizeof(map[0]) * offset, sizeof(map[0]) * size, 0, (void**)&map));
        std::copy_n(data, size, map);
        vkUnmapMemory(m_vkDevice, vtxMem);
    }
};

struct Texture {
    std::vector<std::size_t> totalImageMemSizes{};
    std::vector<VkDeviceMemory> texMemory{};// { VK_NULL_HANDLE };
    VkImage texImage{ VK_NULL_HANDLE };
    VkDevice m_vkDevice{ VK_NULL_HANDLE };
    VkImageLayout m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    inline bool IsValid() const { return texImage != VK_NULL_HANDLE && totalImageMemSizes.size() > 0; }

    inline Texture() noexcept = default;

    void Clear()
    {
        if (m_vkDevice != VK_NULL_HANDLE)
        {
            if (texImage != VK_NULL_HANDLE) {
                vkDestroyImage(m_vkDevice, texImage, nullptr);
            }
            for (auto tm : texMemory) {
                if (tm != VK_NULL_HANDLE)
                    vkFreeMemory(m_vkDevice, tm, nullptr);
            }
        }
        totalImageMemSizes.clear();
        texMemory.clear();        
        texImage = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
        m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    inline ~Texture() {
        Clear();
    }

    inline Texture(const Texture& other) noexcept = delete;
    inline Texture& operator=(const Texture& other) noexcept = delete;

    inline Texture(Texture&& other) noexcept
    : Texture()
    {
        using std::swap;
        totalImageMemSizes = std::move(other.totalImageMemSizes);
        texMemory = std::move(other.texMemory);
        swap(texImage, other.texImage);
        swap(m_vkDevice, other.m_vkDevice);
        swap(m_vkLayout, other.m_vkLayout);
    }

    Texture& operator=(Texture&& other) noexcept {
        if (&other == this) {
            return *this;
        }
        Clear();
        using std::swap;
        totalImageMemSizes = std::move(other.totalImageMemSizes);
        texMemory = std::move(other.texMemory);
        swap(texImage, other.texImage);
        swap(m_vkDevice, other.m_vkDevice);
        swap(m_vkLayout, other.m_vkLayout);
        return *this;
    }

    void Create
    (
        VkDevice device, MemoryAllocator* memAllocator,
        const std::uint32_t width, const std::uint32_t height, const VkFormat format,
        const VkImageTiling imageTiling = VK_IMAGE_TILING_OPTIMAL,
        const VkImageCreateFlags flags = 0
    )
    {
        m_vkDevice = device;

        const VkExtent2D size = { width, height };
        const VkImageCreateInfo imageInfo { 
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {
                .width = size.width,
                .height = size.height,
                .depth = 1
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,//(VkSampleCountFlagBits)swapchainCreateInfo.sampleCount,
            .tiling = imageTiling,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };
        CHECK_VKCMD(vkCreateImage(device, &imageInfo, nullptr, &texImage));

        VkMemoryRequirements memRequirements{};
        vkGetImageMemoryRequirements(device, texImage, &memRequirements);
        totalImageMemSizes.push_back(memRequirements.size);
        VkDeviceMemory tm = VK_NULL_HANDLE;
        memAllocator->Allocate(memRequirements, &tm, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        texMemory.push_back(tm);
        CHECK_VKCMD(vkBindImageMemory(device, texImage, tm, 0));
    }

    void CreateExported
    (
        VkDevice device, MemoryAllocator* memAllocator,
        const std::uint32_t width, const std::uint32_t height, const VkFormat format,
        const VkImageTiling imageTiling = VK_IMAGE_TILING_OPTIMAL,
        const VkImageCreateFlags flags = 0,
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        const VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    )
    {
        m_vkDevice = device;
        assert(m_vkDevice != VK_NULL_HANDLE);

        constexpr const VkExternalMemoryImageCreateInfo vkExternalMemImageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = nullptr,
#ifdef _WIN64
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
#else
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
#endif
        };
        const VkExtent2D size = { width, height };
        const VkImageCreateInfo imageInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &vkExternalMemImageCreateInfo,
            .flags = flags,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent {
                .width = size.width,
                .height = size.height,
                .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,//(VkSampleCountFlagBits)swapchainCreateInfo.sampleCount,
            .tiling = imageTiling,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = m_vkLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        CHECK_VKCMD(vkCreateImage(device, &imageInfo, nullptr, &texImage));

        const bool disjointed = (flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0;
        if (!disjointed)
        {
            VkMemoryRequirements memRequirements{};
            vkGetImageMemoryRequirements(device, texImage, &memRequirements);

#ifdef _WIN64
            const WindowsSecurityAttributes winSecurityAttributes{};
            const VkExportMemoryWin32HandleInfoKHR vulkanExportMemoryWin32HandleInfoKHR {
                .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .pAttributes = &winSecurityAttributes,
                .dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                .name = (LPCWSTR)nullptr,
            };
#endif
            const VkExportMemoryAllocateInfoKHR vulkanExportMemoryAllocateInfoKHR {
                .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
#ifdef _WIN64
                .pNext = /*IsWindows8OrGreater()*/ true ? &vulkanExportMemoryWin32HandleInfoKHR : nullptr,
                .handleTypes = /*IsWindows8OrGreater()*/ true
                    ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
#else
                .pNext = nullptr,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
            };
            VkMemoryRequirements vkMemoryRequirements = {};
            vkGetImageMemoryRequirements(device, texImage, &vkMemoryRequirements);
            totalImageMemSizes.push_back(vkMemoryRequirements.size);

            VkDeviceMemory tm = VK_NULL_HANDLE;
            memAllocator->Allocate(memRequirements, &tm, properties, &vulkanExportMemoryAllocateInfoKHR);
            CHECK_VKCMD(vkBindImageMemory(device, texImage, tm, 0));

            texMemory.push_back(tm);
        }
        else
        {
            const auto AllocateDisjointed = [&](const VkImageAspectFlagBits aspectPlane, std::size_t& totalImageMemSize) -> VkDeviceMemory
            {
                VkImagePlaneMemoryRequirementsInfo imagePlaneMemoryRequirementsInfo {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
                    .pNext = nullptr,
                    .planeAspect = aspectPlane
                };
                const VkImageMemoryRequirementsInfo2 imageMemoryRequirementsInfo2 {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                    .pNext = &imagePlaneMemoryRequirementsInfo,
                    .image = texImage,
                };
                // Get memory requirement for each plane
                VkMemoryRequirements2 memoryRequirements2 {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                    .pNext = &imagePlaneMemoryRequirementsInfo,
                };
                vkGetImageMemoryRequirements2(device, &imageMemoryRequirementsInfo2, &memoryRequirements2);

#ifdef _WIN64
                const WindowsSecurityAttributes winSecurityAttributes{};
                const VkExportMemoryWin32HandleInfoKHR vulkanExportMemoryWin32HandleInfoKHR {
                    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                    .pNext = nullptr,
                    .pAttributes = &winSecurityAttributes,
                    .dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    .name = (LPCWSTR)nullptr,
                };
#endif
                const VkExportMemoryAllocateInfoKHR vulkanExportMemoryAllocateInfoKHR {
                    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
#ifdef _WIN64
                    .pNext = /*IsWindows8OrGreater()*/ true ? &vulkanExportMemoryWin32HandleInfoKHR : nullptr,
                    .handleTypes = /*IsWindows8OrGreater()*/ true
                        ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
                        : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
#else
                    .pNext = nullptr,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
                };
                totalImageMemSize = memoryRequirements2.memoryRequirements.size;

                VkDeviceMemory disjointMemoryPlane = VK_NULL_HANDLE;
                memAllocator->Allocate(memoryRequirements2.memoryRequirements, &disjointMemoryPlane, properties, &vulkanExportMemoryAllocateInfoKHR);

                return disjointMemoryPlane;
            };

            std::size_t totalImageMemSize = 0;
            auto disjointMemoryPlane = AllocateDisjointed(VK_IMAGE_ASPECT_PLANE_0_BIT, totalImageMemSize);
            CHECK(disjointMemoryPlane);
            texMemory.push_back(disjointMemoryPlane);
            totalImageMemSizes.push_back(totalImageMemSize);

            totalImageMemSize = 0;
            disjointMemoryPlane = AllocateDisjointed(VK_IMAGE_ASPECT_PLANE_1_BIT, totalImageMemSize);
            CHECK(disjointMemoryPlane);
            texMemory.push_back(disjointMemoryPlane);
            totalImageMemSizes.push_back(totalImageMemSize);

            constexpr const std::array<const VkBindImagePlaneMemoryInfo,2> bindImagePlaneMemoryInfo{
                VkBindImagePlaneMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
                    .pNext = nullptr,
                    .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT
                },
                VkBindImagePlaneMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
                    .pNext = nullptr,
                    .planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT
                },
            };
            const std::array<const VkBindImageMemoryInfo, 2> bindImageMemoryInfo {
                VkBindImageMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &bindImagePlaneMemoryInfo[0],
                    .image = texImage,
                    .memory = texMemory[0],
                    .memoryOffset = 0
                },
                VkBindImageMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &bindImagePlaneMemoryInfo[1],
                    .image = texImage,
                    .memory = texMemory[1],
                    .memoryOffset = 0
                },
            };
            CHECK_VKCMD(vkBindImageMemory2(device, (std::uint32_t)bindImageMemoryInfo.size(), bindImageMemoryInfo.data()));
        }
    }

#ifdef XR_USE_PLATFORM_ANDROID
    using AHBufferFormatProperties = VkAndroidHardwareBufferFormatPropertiesANDROID;
    void CreateAHardwareBufferImported
    (
        VkDevice vkDevice,
        VkInstance vkinstance,
        MemoryAllocator* memAllocator,
        AHardwareBuffer* buffer,
        const std::uint32_t width,
        const std::uint32_t height,
        //const bool useExternalFormat = false
        AHBufferFormatProperties& formatInfo
    )
    {
        m_vkDevice = vkDevice;

        formatInfo = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
            .pNext = nullptr,
            .format = VK_FORMAT_UNDEFINED,
            .formatFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        };
        VkAndroidHardwareBufferPropertiesANDROID properties {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
            .pNext = &formatInfo
        };        
        const auto vkGetAndroidHardwareBufferPropertiesANDROID =
            (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetInstanceProcAddr(
                vkinstance, "vkGetAndroidHardwareBufferPropertiesANDROID");
        CHECK(vkGetAndroidHardwareBufferPropertiesANDROID != nullptr);
        vkGetAndroidHardwareBufferPropertiesANDROID(vkDevice, buffer, &properties);

        const VkExternalFormatANDROID externalFormat {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
            .pNext = nullptr,
            .externalFormat = formatInfo.externalFormat
        };
        const VkExternalMemoryImageCreateInfo externalCreateInfo {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = formatInfo.format != VK_FORMAT_UNDEFINED ?
                        nullptr : &externalFormat,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
        };
        const VkImageCreateInfo imageInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &externalCreateInfo,
            .flags = 0u,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = formatInfo.format,
            .extent { width, height, 1u },
            .mipLevels = 1u,
            .arrayLayers = 1u,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        CHECK_VKCMD(vkCreateImage(vkDevice, &imageInfo, nullptr, &texImage));

        const VkImportAndroidHardwareBufferInfoANDROID androidHardwareBufferInfo {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .pNext = nullptr,
            .buffer = buffer
        };
        const VkMemoryDedicatedAllocateInfo memoryAllocateInfo {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &androidHardwareBufferInfo,
            .image = texImage,
            .buffer = VK_NULL_HANDLE            
        };
        const VkMemoryRequirements memRequirements {
            .size = properties.allocationSize,
            .alignment = 0,
            .memoryTypeBits = properties.memoryTypeBits            
        };
        totalImageMemSizes.push_back(memRequirements.size);
        VkDeviceMemory tm = VK_NULL_HANDLE;
        memAllocator->Allocate(memRequirements, &tm, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryAllocateInfo);
        texMemory.push_back(tm);

        const VkBindImageMemoryInfo bindImageInfo {
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .pNext = nullptr,
            .image = texImage,
            .memory = tm,
            .memoryOffset = 0,
        };
        CHECK_VKCMD(vkBindImageMemory2(vkDevice, 1, &bindImageInfo));
    }
#else
    struct AHBufferFormatProperties;
    void CreateAHardwareBufferImported
    (
        VkDevice /*vkDevice*/,
        VkInstance /*vkinstance*/,
        MemoryAllocator* /*memAllocator*/,
        struct AHardwareBuffer* /*buffer*/,
        const std::uint32_t /*width*/,
        const std::uint32_t /*height*/,
        struct AHBufferFormatProperties&
    )
    {}
#endif

#ifdef XR_USE_PLATFORM_WIN32
    void CreateImportedD3D11Texture
    (
        VkPhysicalDevice phyDevice, VkDevice device, MemoryAllocator* memAllocator,
        const HANDLE d3d11Tex, const std::uint32_t width, const std::uint32_t height, const VkFormat format,
        const VkImageTiling imageTiling = VK_IMAGE_TILING_OPTIMAL,
        const VkImageCreateFlags flags = 0,
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        const VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    )
    {
        m_vkDevice = device;

        constexpr const VkPhysicalDeviceExternalImageFormatInfo physicalDeviceExternalImageFormatInfo {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = nullptr,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
        };
         VkPhysicalDeviceImageFormatInfo2 physicalDeviceImageFormatInfo2 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &physicalDeviceExternalImageFormatInfo,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = imageTiling,
            .usage = usage,
            .flags = flags
        };
        VkExternalImageFormatProperties externalImageFormatProperties {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
            .pNext = nullptr
        };
        VkImageFormatProperties2 imageFormatProperties2 {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &externalImageFormatProperties
        };
        CHECK_VKCMD(vkGetPhysicalDeviceImageFormatProperties2(phyDevice, &physicalDeviceImageFormatInfo2, &imageFormatProperties2));
        assert(externalImageFormatProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
        assert(externalImageFormatProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
        assert(externalImageFormatProperties.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);
        
        constexpr const VkExternalMemoryImageCreateInfo ExternalMemoryImageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
        };
        const VkExtent3D Extent = { width, height, 1 };
        const VkImageCreateInfo ImageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ExternalMemoryImageCreateInfo,
            .flags = flags,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = Extent,
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = imageTiling,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        CHECK_VKCMD(vkCreateImage(device, &ImageCreateInfo, nullptr, &texImage));

        const bool disjointed = (flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0;

        if (!disjointed)
        {
            VkMemoryDedicatedRequirements MemoryDedicatedRequirements {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
                .pNext = nullptr
            };
            VkMemoryRequirements2 MemoryRequirements2 {
                .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                .pNext = &MemoryDedicatedRequirements
            };
            const VkImageMemoryRequirementsInfo2 ImageMemoryRequirementsInfo2 {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = texImage
            };
            // WARN: Memory access violation unless validation instance layer is enabled, otherwise success but...
            vkGetImageMemoryRequirements2(device, &ImageMemoryRequirementsInfo2, &MemoryRequirements2);
            //       ... if we happen to be here, MemoryRequirements2 is empty
            VkMemoryRequirements& MemoryRequirements = MemoryRequirements2.memoryRequirements;

            const VkMemoryDedicatedAllocateInfo MemoryDedicatedAllocateInfo {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = nullptr,
                .image = texImage,
                .buffer = VK_NULL_HANDLE
            };
            const VkImportMemoryWin32HandleInfoKHR ImportMemoryWin32HandleInfo {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                .pNext = &MemoryDedicatedAllocateInfo,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                .handle = d3d11Tex,
                .name = nullptr
            };
            VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
            memAllocator->Allocate(MemoryRequirements, &ImageMemory, properties, &ImportMemoryWin32HandleInfo);
            CHECK(ImageMemory != VK_NULL_HANDLE);

            const VkBindImageMemoryInfo bindImageMemoryInfo{
                .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                .pNext = nullptr,
                .image = texImage,
                .memory = ImageMemory,
                .memoryOffset = 0
            };
            CHECK_VKCMD(vkBindImageMemory2(device, 1, &bindImageMemoryInfo));

            texMemory.push_back(ImageMemory);
            totalImageMemSizes.push_back(MemoryRequirements.size);
        }
        else
        {
            const auto AllocateDisjointed = [&](const VkImageAspectFlagBits aspectPlane, std::size_t& totalImageMemSize) -> VkDeviceMemory
            {
                const VkImagePlaneMemoryRequirementsInfo imagePlaneMemoryRequirementsInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
                    .pNext = nullptr,
                    .planeAspect = aspectPlane
                };
                const VkImageMemoryRequirementsInfo2 ImageMemoryRequirementsInfo2{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                    .pNext = &imagePlaneMemoryRequirementsInfo,
                    .image = texImage,
                };
                VkMemoryDedicatedRequirements MemoryDedicatedRequirements{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
                    .pNext = nullptr
                };
                VkMemoryRequirements2 MemoryRequirements2{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                    .pNext = &MemoryDedicatedRequirements
                };
                vkGetImageMemoryRequirements2(device, &ImageMemoryRequirementsInfo2, &MemoryRequirements2);
                VkMemoryRequirements& MemoryRequirements = MemoryRequirements2.memoryRequirements;
                totalImageMemSize = MemoryRequirements.size;

                const VkMemoryDedicatedAllocateInfo MemoryDedicatedAllocateInfo{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .image = VK_NULL_HANDLE,//texImage,
                    .buffer = VK_NULL_HANDLE
                };
                const VkImportMemoryWin32HandleInfoKHR ImportMemoryWin32HandleInfo{
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                    .pNext = &MemoryDedicatedAllocateInfo,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                    .handle = d3d11Tex,
                    .name = nullptr
                };
                VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
                memAllocator->Allocate(MemoryRequirements, &ImageMemory, properties, &ImportMemoryWin32HandleInfo);
                CHECK(ImageMemory != VK_NULL_HANDLE);

                return ImageMemory;
            };

            std::size_t totalImageMemSize = 0;
            auto disjointMemoryPlane = AllocateDisjointed(VK_IMAGE_ASPECT_PLANE_0_BIT, totalImageMemSize);
            CHECK(disjointMemoryPlane);
            texMemory.push_back(disjointMemoryPlane);
            totalImageMemSizes.push_back(totalImageMemSize);

            totalImageMemSize = 0;
            disjointMemoryPlane = AllocateDisjointed(VK_IMAGE_ASPECT_PLANE_1_BIT, totalImageMemSize);
            CHECK(disjointMemoryPlane);
            texMemory.push_back(disjointMemoryPlane);
            totalImageMemSizes.push_back(totalImageMemSize);

            constexpr const std::array<const VkBindImagePlaneMemoryInfo,2> bindImagePlaneMemoryInfo{
                VkBindImagePlaneMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
                    .pNext = nullptr,
                    .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT
                },
                VkBindImagePlaneMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
                    .pNext = nullptr,
                    .planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT
                },
            };
            const std::array<const VkBindImageMemoryInfo, 2> bindImageMemoryInfo{
                VkBindImageMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &bindImagePlaneMemoryInfo[0],
                    .image = texImage,
                    .memory = texMemory[0],
                    .memoryOffset = 0
                },
                VkBindImageMemoryInfo {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &bindImagePlaneMemoryInfo[1],
                    .image = texImage,
                    .memory = texMemory[1],
                    .memoryOffset = 0
                },
            };
            CHECK_VKCMD(vkBindImageMemory2(device, (std::uint32_t)bindImageMemoryInfo.size(), bindImageMemoryInfo.data()));
        }
    }
#endif

    void TransitionLayout(CmdBuffer& cmdBuffer, const VkImageLayout newLayout)
    {
        if (newLayout == m_vkLayout)
            return;

        const auto oldLayout = m_vkLayout;
        VkImageMemoryBarrier barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };        
        VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else {
            //throw std::invalid_argument("unsupported layout transition!");
            Log::Write(Log::Level::Warning, Fmt("unsupported layout transition, old layout: %u target layout: %u, falling back to src/dst VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT", 
                oldLayout, newLayout));
        }

        vkCmdPipelineBarrier
        (
            cmdBuffer.buf, sourceStage, destinationStage, 0, 0,
            nullptr, 0, nullptr, 1, &barrier
        );

        m_vkLayout = newLayout;
    }
};

// RenderPass wrapper
struct RenderPass {
    VkFormat colorFmt{};
    VkFormat depthFmt{};
    VkRenderPass pass{VK_NULL_HANDLE};
    std::uint32_t arraySize = 0;

    RenderPass() = default;

    bool Create(VkDevice device, VkFormat aColorFmt, VkFormat aDepthFmt, const std::uint32_t arraySizeParam) {
        m_vkDevice = device;
        colorFmt = aColorFmt;
        depthFmt = aDepthFmt;
        arraySize = arraySizeParam;
        assert(arraySize > 0);
        const bool isMultiView = arraySize > 1;

        // Subpass dependencies for layout transitions
        constexpr static const std::array<const VkSubpassDependency, 1> dependencies {
            VkSubpassDependency {
                .srcSubpass      = VK_SUBPASS_EXTERNAL,
                .dstSubpass      = 0,
                .srcStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                .dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask   = VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
            },
        };
        //
        //  Setup multiview info for the renderpass
        //
        //  Bit mask that specifies which view rendering is broadcast to
        //  0011 = Broadcast to first and second view (layer)
        //
        constexpr static const std::uint32_t viewMask = 0b00000011;
        //
        //  Bit mask that specifies correlation between views
        //  An implementation may use this for optimizations (concurrent render)
        //
        constexpr static const std::uint32_t correlationMask = 0b00000011;

        constexpr static const VkRenderPassMultiviewCreateInfoKHR renderPassMultiviewCI {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO_KHR,
            .pNext = nullptr,
            .subpassCount = 1,
            .pViewMasks = &viewMask,
            .correlationMaskCount = 1,
            .pCorrelationMasks = &correlationMask,
        };

        VkSubpassDescription subpass = {
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS
        };
        std::array<VkAttachmentDescription, 2> at = {};
        VkRenderPassCreateInfo rpInfo{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext = isMultiView ? &renderPassMultiviewCI : nullptr,
            .attachmentCount = 0,
            .pAttachments = at.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = isMultiView ? (std::uint32_t)dependencies.size() : 0,
            .pDependencies = isMultiView ? dependencies.data() : nullptr
        };
        VkAttachmentReference colorRef {
            .attachment = 0,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        if (colorFmt != VK_FORMAT_UNDEFINED) {
            Log::Write(Log::Level::Info, Fmt("setting color frame layout, format: %ld", colorFmt));
            colorRef.attachment = rpInfo.attachmentCount++;
            at[colorRef.attachment] = {
                .format = colorFmt,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,//VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            };
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
        }

        VkAttachmentReference depthRef {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };
        if (depthFmt != VK_FORMAT_UNDEFINED) {
            depthRef.attachment = rpInfo.attachmentCount++;
            at[depthRef.attachment] = {
                .format = depthFmt,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, //VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            };
            subpass.pDepthStencilAttachment = &depthRef;
        }

        CHECK_VKCMD(vkCreateRenderPass(m_vkDevice, &rpInfo, nullptr, &pass));

        return true;
    }

    ~RenderPass() {
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (pass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_vkDevice, pass, nullptr);
            }
        }
        pass = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
    }

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
};

// VkImage + framebuffer wrapper
struct RenderTarget {
    VkImage colorImage{VK_NULL_HANDLE};
    VkImage depthImage{VK_NULL_HANDLE};
    VkImageView colorView{VK_NULL_HANDLE};
    VkImageView depthView{VK_NULL_HANDLE};
    VkFramebuffer fb{VK_NULL_HANDLE};

    RenderTarget() = default;

    ~RenderTarget() {
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(m_vkDevice, fb, nullptr);
            }
            if (colorView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_vkDevice, colorView, nullptr);
            }
            if (depthView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_vkDevice, depthView, nullptr);
            }
        }

        // Note we don't own color/depthImage, it will get destroyed when xrDestroySwapchain is called
        colorImage = VK_NULL_HANDLE;
        depthImage = VK_NULL_HANDLE;
        colorView = VK_NULL_HANDLE;
        depthView = VK_NULL_HANDLE;
        fb = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
    }

    RenderTarget(RenderTarget&& other) noexcept : RenderTarget() {
        using std::swap;
        swap(colorImage, other.colorImage);
        swap(depthImage, other.depthImage);
        swap(colorView, other.colorView);
        swap(depthView, other.depthView);
        swap(fb, other.fb);
        swap(m_vkDevice, other.m_vkDevice);
    }

    RenderTarget& operator=(RenderTarget&& other) noexcept {
        if (&other == this) {
            return *this;
        }
        // Clean up ourselves.
        this->~RenderTarget();
        using std::swap;
        swap(colorImage, other.colorImage);
        swap(depthImage, other.depthImage);
        swap(colorView, other.colorView);
        swap(depthView, other.depthView);
        swap(fb, other.fb);
        swap(m_vkDevice, other.m_vkDevice);
        return *this;
    }

    void Create(VkDevice device, VkImage aColorImage, VkImage aDepthImage, VkExtent2D size, RenderPass& renderPass) {
        m_vkDevice = device;

        colorImage = aColorImage;
        depthImage = aDepthImage;
        assert(renderPass.arraySize > 0);
        const bool isMultiView = renderPass.arraySize > 1;
        const VkImageViewType viewType = isMultiView ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

        uint32_t attachmentCount = 0;
        std::array<VkImageView, 2> attachments{};

        // Create color image view
        if (colorImage != VK_NULL_HANDLE) {
            const VkImageViewCreateInfo colorViewInfo {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = colorImage,
                .viewType = viewType,
                .format = renderPass.colorFmt,
                .components {
                    .r = VK_COMPONENT_SWIZZLE_R,
                    .g = VK_COMPONENT_SWIZZLE_G,
                    .b = VK_COMPONENT_SWIZZLE_B,
                    .a = VK_COMPONENT_SWIZZLE_A
                },
                .subresourceRange {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = renderPass.arraySize
                }
            };
            CHECK_VKCMD(vkCreateImageView(m_vkDevice, &colorViewInfo, nullptr, &colorView));
            attachments[attachmentCount++] = colorView;
        }

        // Create depth image view
        if (depthImage != VK_NULL_HANDLE) {
            const VkImageViewCreateInfo depthViewInfo {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = depthImage,
                .viewType = viewType,
                .format = renderPass.depthFmt,
                .components {
                    .r = VK_COMPONENT_SWIZZLE_R,
                    .g = VK_COMPONENT_SWIZZLE_G,
                    .b = VK_COMPONENT_SWIZZLE_B,
                    .a = VK_COMPONENT_SWIZZLE_A
                },
                .subresourceRange {
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = renderPass.arraySize
                }
            };
            CHECK_VKCMD(vkCreateImageView(m_vkDevice, &depthViewInfo, nullptr, &depthView));
            attachments[attachmentCount++] = depthView;
        }

        const VkFramebufferCreateInfo fbInfo {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .renderPass = renderPass.pass,
            .attachmentCount = attachmentCount,
            .pAttachments = attachments.data(),
            .width = size.width,
            .height = size.height,
            .layers = 1
        };
        CHECK_VKCMD(vkCreateFramebuffer(m_vkDevice, &fbInfo, nullptr, &fb));
    }

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
};

struct alignas(16) ViewProjectionUniform {
    XrMatrix4x4f mvp;
    std::uint32_t ViewID;
};

struct alignas(16) MultiViewProjectionUniform {
    XrMatrix4x4f mvp[2];
};

// Simple vertex MVP xform & color fragment shader layout
struct PipelineLayout {
    VkPipelineLayout layout{VK_NULL_HANDLE};

    inline PipelineLayout() = default;

    inline void Clear() {
        if (m_vkDevice != VK_NULL_HANDLE)
        {
            if (layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_vkDevice, layout, nullptr);
            if (descriptorSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_vkDevice, descriptorSetLayout, nullptr);
            if (textureSampler != VK_NULL_HANDLE)
                vkDestroySampler(m_vkDevice, textureSampler, nullptr);
            if (m_vkinstance != VK_NULL_HANDLE &&
                ycbcrSamplerConversion != VK_NULL_HANDLE) {
#if 1
                const auto fpDestroySamplerYcbcrConversion =
                    (PFN_vkDestroySamplerYcbcrConversion)vkGetInstanceProcAddr(m_vkinstance, "vkDestroySamplerYcbcrConversion");
                if (fpDestroySamplerYcbcrConversion == nullptr) {
                    throw std::runtime_error(
                        "Vulkan: Proc address for \"vkDestroySamplerYcbcrConversion\" not "
                        "found.\n");
                }
#else
                constexpr const PFN_vkDestroySamplerYcbcrConversion fpDestroySamplerYcbcrConversion =
                    vkDestroySamplerYcbcrConversion;
#endif
                fpDestroySamplerYcbcrConversion(m_vkDevice, ycbcrSamplerConversion, nullptr);
            }
        }
        ycbcrSamplerConversion = VK_NULL_HANDLE;
        textureSampler = VK_NULL_HANDLE;
        descriptorSetLayout = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
        m_vkinstance = VK_NULL_HANDLE;
    }

    ~PipelineLayout() {
        Clear();
    }

    // Simple vertex MVP xform & color fragment shader layout
    void Create(VkDevice device, VkInstance vkinstance, const bool isMultiView) {
        CHECK(device != VK_NULL_HANDLE && vkinstance != VK_NULL_HANDLE);
        Clear();
        m_vkDevice = device;
        m_vkinstance = vkinstance;
        // MVP matrix is a push_constant
        const VkPushConstantRange pcr {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = (std::uint32_t)(isMultiView ? sizeof(MultiViewProjectionUniform) : sizeof(ViewProjectionUniform)),
        };
        const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr
        };
        CHECK_VKCMD(vkCreatePipelineLayout(m_vkDevice, &pipelineLayoutCreateInfo, nullptr, &layout));
    }

    bool IsNull() const { return layout == VK_NULL_HANDLE; }

//////////////////////////////////////////////////////////////////////////////////////////
// Begin Video Stream Layout
    VkSamplerYcbcrConversion ycbcrSamplerConversion{ VK_NULL_HANDLE };
    VkSampler textureSampler{ VK_NULL_HANDLE };
    VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };

    void CreateVideoStreamLayout
    (
        const VkSamplerYcbcrConversionCreateInfo& conversionInfo,
        VkDevice device, VkInstance vkinstance, const bool isMultiview
    )
    {
        CHECK(device != VK_NULL_HANDLE && vkinstance != VK_NULL_HANDLE);
        Clear();
        m_vkDevice = device;
        m_vkinstance = vkinstance;
#if 1
        const PFN_vkCreateSamplerYcbcrConversion fpCreateSamplerYcbcrConversion =
            (PFN_vkCreateSamplerYcbcrConversion)vkGetInstanceProcAddr(vkinstance, "vkCreateSamplerYcbcrConversion");
        if (fpCreateSamplerYcbcrConversion == nullptr) {
            throw std::runtime_error(
                "Vulkan: Proc address for \"vkCreateSamplerYcbcrConversion\" not "
                "found.\n");
        }
#else
        constexpr const PFN_vkCreateSamplerYcbcrConversion fpCreateSamplerYcbcrConversion =
            vkCreateSamplerYcbcrConversion;
#endif
        CHECK_VKCMD(fpCreateSamplerYcbcrConversion(m_vkDevice, &conversionInfo, nullptr, &ycbcrSamplerConversion));

        const VkSamplerYcbcrConversionInfo ycbcrConverInfo {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            .pNext = nullptr,
            .conversion = ycbcrSamplerConversion
        };
        const VkSamplerCreateInfo samplerInfo {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = &ycbcrConverInfo,
            .flags = 0,            
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,            
            .anisotropyEnable = VK_FALSE,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = 1.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        CHECK_VKCMD(vkCreateSampler(m_vkDevice, &samplerInfo, nullptr, &textureSampler));
        
        const std::array<const VkDescriptorSetLayoutBinding, 1> layoutBindings {
            VkDescriptorSetLayoutBinding {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = &textureSampler,
            }
        };
        const VkDescriptorSetLayoutCreateInfo layoutInfo {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .bindingCount = (std::uint32_t)layoutBindings.size(),
            .pBindings = layoutBindings.data()
        };
        CHECK_VKCMD(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout));

        static_assert(sizeof(MultiViewProjectionUniform) <= 128);
        static_assert(sizeof(ViewProjectionUniform) <= 128);
        // MVP matrix is a push_constant
        const VkPushConstantRange pcr {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = (std::uint32_t)(isMultiview ? sizeof(MultiViewProjectionUniform) : sizeof(ViewProjectionUniform)),
        };
        CHECK(pcr.size <= 128);
        const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .setLayoutCount = 1,
            .pSetLayouts = &descriptorSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr
        };
        CHECK_VKCMD(vkCreatePipelineLayout(m_vkDevice, &pipelineLayoutCreateInfo, nullptr, &layout));
    }
// Begin Video Stream Layout
//////////////////////////////////////////////////////////////////////////////////////////

    PipelineLayout(const PipelineLayout&) = delete;
    PipelineLayout& operator=(const PipelineLayout&) = delete;
    PipelineLayout(PipelineLayout&&) = delete;
    PipelineLayout& operator=(PipelineLayout&&) = delete;

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    VkInstance m_vkinstance{VK_NULL_HANDLE};
};

// Pipeline wrapper for rendering pipeline state
struct Pipeline {
    VkPipeline pipe{VK_NULL_HANDLE};
    static constexpr const VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    std::vector<VkDynamicState> dynamicStateEnables;

    Pipeline() = default;
    ~Pipeline() { Clear(); }

    //void Dynamic(VkDynamicState state) { dynamicStateEnables.emplace_back(state); }

    void Create(VkDevice device, VkExtent2D size, const PipelineLayout& layout, const RenderPass& rp, const ShaderProgram& sp,
                const VertexBufferBase& vb) {
        m_vkDevice = device;

        const VkPipelineDynamicStateCreateInfo dynamicState {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .dynamicStateCount = (uint32_t)dynamicStateEnables.size(),
            .pDynamicStates = dynamicStateEnables.data()
        };

        const VkPipelineVertexInputStateCreateInfo vi {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vb.bindDesc,
            .vertexAttributeDescriptionCount = (uint32_t)vb.attrDesc.size(),
            .pVertexAttributeDescriptions = vb.attrDesc.data()
        };

        constexpr const VkPipelineInputAssemblyStateCreateInfo ia {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .topology = topology,
            .primitiveRestartEnable = VK_FALSE
        };

        constexpr const VkPipelineRasterizationStateCreateInfo rs {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0,
            .depthBiasClamp = 0,
            .depthBiasSlopeFactor = 0,
            .lineWidth = 1.0f,
        };

        constexpr const VkPipelineColorBlendAttachmentState attachState {
            .blendEnable = 0,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo cb {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_NO_OP,
            .attachmentCount = 1,
            .pAttachments = &attachState,
            .blendConstants {
                1.0f,
                1.0f,
                1.0f,
                1.0f,
            }
        };
        const VkRect2D scissor = {{0, 0}, size};
#if defined(ORIGIN_BOTTOM_LEFT)
        // Flipped view so origin is bottom-left like GL (requires VK_KHR_maintenance1)
        const VkViewport viewport = {0.0f, (float)size.height, (float)size.width, -(float)size.height, 0.0f, 1.0f};
#else
        // Will invert y after projection
        const VkViewport viewport = {0.0f, 0.0f, (float)size.width, (float)size.height, 0.0f, 1.0f};
#endif
        const VkPipelineViewportStateCreateInfo vp {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };
        const VkPipelineDepthStencilStateCreateInfo ds {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front {
                .failOp = VK_STENCIL_OP_KEEP,
                .passOp = VK_STENCIL_OP_KEEP,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp = VK_COMPARE_OP_ALWAYS,
            },
            .back = ds.front,
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
        };
        constexpr const VkPipelineMultisampleStateCreateInfo ms {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkGraphicsPipelineCreateInfo pipeInfo {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .stageCount = (uint32_t)sp.shaderInfo.size(),
            .pStages = sp.shaderInfo.data(),
            .pVertexInputState = &vi,
            .pInputAssemblyState = &ia,
            .pTessellationState = nullptr,
            .pViewportState = &vp,
            .pRasterizationState = &rs,
            .pMultisampleState = &ms,
            .pDepthStencilState = &ds,
            .pColorBlendState = &cb,
            .pDynamicState = dynamicState.dynamicStateCount > 0 ? &dynamicState : nullptr,
            .layout = layout.layout,
            .renderPass = rp.pass,
            .subpass = 0,
        };
        CHECK_VKCMD(vkCreateGraphicsPipelines(m_vkDevice, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipe));
    }

    void Clear() {
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (pipe != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_vkDevice, pipe, nullptr);
            }
        }
        pipe = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
        dynamicStateEnables.clear();
    }

private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
};

struct DepthBuffer {
    VkDeviceMemory depthMemory{VK_NULL_HANDLE};
    VkImage depthImage{VK_NULL_HANDLE};

    DepthBuffer() = default;

    ~DepthBuffer() {
        if (m_vkDevice != VK_NULL_HANDLE) {
            if (depthImage != VK_NULL_HANDLE) {
                vkDestroyImage(m_vkDevice, depthImage, nullptr);
            }
            if (depthMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_vkDevice, depthMemory, nullptr);
            }
        }
        depthImage = VK_NULL_HANDLE;
        depthMemory = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
        m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    DepthBuffer(DepthBuffer&& other) noexcept : DepthBuffer() {
        using std::swap;

        swap(depthImage, other.depthImage);
        swap(depthMemory, other.depthMemory);
        swap(m_vkDevice, other.m_vkDevice);
        swap(m_vkLayout, other.m_vkLayout);
    }
    DepthBuffer& operator=(DepthBuffer&& other) noexcept {
        if (&other == this) {
            return *this;
        }
        // clean up self
        this->~DepthBuffer();
        using std::swap;

        swap(depthImage, other.depthImage);
        swap(depthMemory, other.depthMemory);
        swap(m_vkDevice, other.m_vkDevice);
        swap(m_vkLayout, other.m_vkLayout);
        return *this;
    }

    void Create(VkDevice device, MemoryAllocator* memAllocator, VkFormat depthFormat,
                const XrSwapchainCreateInfo& swapchainCreateInfo) {
        m_vkDevice = device;
        assert(swapchainCreateInfo.arraySize > 0);
        const VkExtent2D size = {swapchainCreateInfo.width, swapchainCreateInfo.height};
        // Create a D32 depthbuffer
        const VkImageCreateInfo imageInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = depthFormat,
            .extent {
                .width = size.width,
                .height = size.height,
                .depth = 1
             },
            .mipLevels = 1,
            .arrayLayers = swapchainCreateInfo.arraySize,
            .samples = (VkSampleCountFlagBits)swapchainCreateInfo.sampleCount,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        CHECK_VKCMD(vkCreateImage(device, &imageInfo, nullptr, &depthImage));

        VkMemoryRequirements memRequirements{};
        vkGetImageMemoryRequirements(device, depthImage, &memRequirements);
        memAllocator->Allocate(memRequirements, &depthMemory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK_VKCMD(vkBindImageMemory(device, depthImage, depthMemory, 0));
    }

    void TransitionLayout(CmdBuffer* cmdBuffer, VkImageLayout newLayout) {
        if (newLayout == m_vkLayout)
            return;
        const VkImageMemoryBarrier depthBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .oldLayout = m_vkLayout,
            .newLayout = newLayout,
            .image = depthImage,
            .subresourceRange { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmdBuffer->buf, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &depthBarrier);

        m_vkLayout = newLayout;
    }

    DepthBuffer(const DepthBuffer&) = delete;
    DepthBuffer& operator=(const DepthBuffer&) = delete;

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    VkImageLayout m_vkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct SwapchainImageContext {
    SwapchainImageContext(XrStructureType _swapchainImageType) : swapchainImageType(_swapchainImageType) {}

    // A packed array of XrSwapchainImageVulkan2KHR's for xrEnumerateSwapchainImages
    std::vector<XrSwapchainImageVulkan2KHR> swapchainImages;
    std::vector<RenderTarget> renderTarget;
    VkExtent2D size{};
    DepthBuffer depthBuffer{};
    RenderPass rp{};
    Pipeline pipe{};
    XrStructureType swapchainImageType;
    std::uint32_t arraySize = 0;

    SwapchainImageContext() = default;

    std::vector<XrSwapchainImageBaseHeader*> Create
    (
        VkDevice device, MemoryAllocator* memAllocator, uint32_t capacity,
        const XrSwapchainCreateInfo& swapchainCreateInfo, const PipelineLayout& layout,
        const ShaderProgram& sp, const VertexBuffer<Geometry::Vertex>& vb
    )
    {
        m_vkDevice = device;
        arraySize = swapchainCreateInfo.arraySize;
        assert(arraySize > 0);
        size = {swapchainCreateInfo.width, swapchainCreateInfo.height};

        const VkFormat colorFormat = static_cast<VkFormat>(swapchainCreateInfo.format);
        const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        // XXX handle swapchainCreateInfo.sampleCount
        
        depthBuffer.Create(m_vkDevice, memAllocator, depthFormat, swapchainCreateInfo);
        rp.Create(m_vkDevice, colorFormat, depthFormat, arraySize);
        pipe.Create(m_vkDevice, size, layout, rp, sp, vb);

        swapchainImages.resize(capacity);
        renderTarget.resize(capacity);
        std::vector<XrSwapchainImageBaseHeader*> bases(capacity);
        for (uint32_t i = 0; i < capacity; ++i) {
            swapchainImages[i] = {
                .type = swapchainImageType,
                .next = nullptr,
                .image = VK_NULL_HANDLE
            };
            bases[i] = reinterpret_cast<XrSwapchainImageBaseHeader*>(&swapchainImages[i]);
        }

        return bases;
    }

    uint32_t ImageIndex(const XrSwapchainImageBaseHeader* swapchainImageHeader) const {
        const auto p = reinterpret_cast<const XrSwapchainImageVulkan2KHR*>(swapchainImageHeader);
        return (uint32_t)(p - &swapchainImages[0]);
    }

    inline void BindRenderTarget(const std::uint32_t index, VkRenderPassBeginInfo& renderPassBeginInfo) {
        if (renderTarget[index].fb == VK_NULL_HANDLE) {
            renderTarget[index].Create(m_vkDevice, swapchainImages[index].image, depthBuffer.depthImage, size, rp);
        }
        renderPassBeginInfo.renderPass = rp.pass;
        renderPassBeginInfo.framebuffer = renderTarget[index].fb;
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = size;
    }

   private:
    VkDevice m_vkDevice{VK_NULL_HANDLE};
};

#if defined(USE_MIRROR_WINDOW)
// Swapchain
struct Swapchain {
    VkFormat format{VK_FORMAT_B8G8R8A8_SRGB};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFence readyFence{VK_NULL_HANDLE};
    VkFence presentFence{VK_NULL_HANDLE};
    static const uint32_t maxImages = 4;
    uint32_t swapchainCount = 0;
    uint32_t renderImageIdx = 0;
    VkImage image[maxImages]{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};

    Swapchain() {}
    ~Swapchain() { Release(); }

    void Create(VkInstance instance, VkPhysicalDevice physDevice, VkDevice device, uint32_t queueFamilyIndex);
    void Prepare(VkCommandBuffer buf);
    void Wait();
    void Acquire(VkSemaphore readySemaphore = VK_NULL_HANDLE);
    void Present(VkQueue queue, VkSemaphore drawComplete = VK_NULL_HANDLE);
    void Release() {
        if (m_vkDevice) {
            // Flush any pending Present() calls which are using the fence
            Wait();
            if (swapchain) vkDestroySwapchainKHR(m_vkDevice, swapchain, nullptr);
            if (readyFence) vkDestroyFence(m_vkDevice, readyFence, nullptr);
        }

        if (m_vkInstance && surface) vkDestroySurfaceKHR(m_vkInstance, surface, nullptr);

        readyFence = VK_NULL_HANDLE;
        presentFence = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < swapchainCount; ++i) {
            image[i] = VK_NULL_HANDLE;
        }
        swapchainCount = 0;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (hWnd) {
            DestroyWindow(hWnd);
            hWnd = nullptr;
            UnregisterClassW(L"alxr-client", hInst);
        }
#endif

        m_vkDevice = nullptr;
    }
    void Recreate() {
        Release();
        Create(m_vkInstance, m_vkPhysicalDevice, m_vkDevice, m_queueFamilyIndex);
    }

   private:
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    HINSTANCE hInst{NULL};
    HWND hWnd{NULL};
#endif
    const VkExtent2D size{640, 480};
    VkInstance m_vkInstance{VK_NULL_HANDLE};
    VkPhysicalDevice m_vkPhysicalDevice{VK_NULL_HANDLE};
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    uint32_t m_queueFamilyIndex = 0;
};

void Swapchain::Create(VkInstance instance, VkPhysicalDevice physDevice, VkDevice device, uint32_t queueFamilyIndex) {
    m_vkInstance = instance;
    m_vkPhysicalDevice = physDevice;
    m_vkDevice = device;
    m_queueFamilyIndex = queueFamilyIndex;

// Create a WSI surface for the window:
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    hInst = GetModuleHandle(NULL);

    WNDCLASSW wc{};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.cbWndExtra = sizeof(this);
    wc.hInstance = hInst;
    wc.lpszClassName = L"alxr-client";
    RegisterClassW(&wc);

// adjust the window size and show at InitDevice time
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    // Make sure we're 1:1 for HMD pixels
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    RECT rect{0, 0, (LONG)size.width, (LONG)size.height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, false);
    hWnd = CreateWindowW(wc.lpszClassName, L"hello_xr (Vulkan)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         rect.right - rect.left, rect.bottom - rect.top, 0, 0, hInst, 0);
    assert(hWnd != NULL);

    SetWindowLongPtr(hWnd, 0, LONG_PTR(this));

    VkWin32SurfaceCreateInfoKHR surfCreateInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfCreateInfo.flags = 0;
    surfCreateInfo.hinstance = hInst;
    surfCreateInfo.hwnd = hWnd;
    CHECK_VKCMD(vkCreateWin32SurfaceKHR(m_vkInstance, &surfCreateInfo, nullptr, &surface));
#else
#error CreateSurface not supported on this OS
#endif  // defined(VK_USE_PLATFORM_WIN32_KHR)

    VkSurfaceCapabilitiesKHR surfCaps;
    CHECK_VKCMD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_vkPhysicalDevice, surface, &surfCaps));
    CHECK(surfCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    uint32_t surfFmtCount = 0;
    CHECK_VKCMD(vkGetPhysicalDeviceSurfaceFormatsKHR(m_vkPhysicalDevice, surface, &surfFmtCount, nullptr));
    std::vector<VkSurfaceFormatKHR> surfFmts(surfFmtCount);
    CHECK_VKCMD(vkGetPhysicalDeviceSurfaceFormatsKHR(m_vkPhysicalDevice, surface, &surfFmtCount, &surfFmts[0]));
    uint32_t foundFmt;
    for (foundFmt = 0; foundFmt < surfFmtCount; ++foundFmt) {
        if (surfFmts[foundFmt].format == format) break;
    }

    CHECK(foundFmt < surfFmtCount);

    uint32_t presentModeCount = 0;
    CHECK_VKCMD(vkGetPhysicalDeviceSurfacePresentModesKHR(m_vkPhysicalDevice, surface, &presentModeCount, nullptr));
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    CHECK_VKCMD(vkGetPhysicalDeviceSurfacePresentModesKHR(m_vkPhysicalDevice, surface, &presentModeCount, &presentModes[0]));

    // Do not use VSYNC for the mirror window, but Nvidia doesn't support IMMEDIATE so fall back to MAILBOX
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    for (uint32_t i = 0; i < presentModeCount; ++i) {
        if ((presentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) || (presentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)) {
            presentMode = presentModes[i];
            break;
        }
    }

    VkBool32 presentable = false;
    CHECK_VKCMD(vkGetPhysicalDeviceSurfaceSupportKHR(m_vkPhysicalDevice, m_queueFamilyIndex, surface, &presentable));
    CHECK(presentable);

    VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainInfo.flags = 0;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = surfCaps.minImageCount;
    swapchainInfo.imageFormat = format;
    swapchainInfo.imageColorSpace = surfFmts[foundFmt].colorSpace;
    swapchainInfo.imageExtent = size;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.queueFamilyIndexCount = 0;
    swapchainInfo.pQueueFamilyIndices = nullptr;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = true;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
    CHECK_VKCMD(vkCreateSwapchainKHR(m_vkDevice, &swapchainInfo, nullptr, &swapchain));

    // Fence to throttle host on Acquire
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK_VKCMD(vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &readyFence));

    swapchainCount = 0;
    CHECK_VKCMD(vkGetSwapchainImagesKHR(m_vkDevice, swapchain, &swapchainCount, nullptr));
    assert(swapchainCount < maxImages);
    CHECK_VKCMD(vkGetSwapchainImagesKHR(m_vkDevice, swapchain, &swapchainCount, image));
    if (swapchainCount > maxImages) {
        Log::Write(Log::Level::Info,
                   "Reducing swapchain length from " + std::to_string(swapchainCount) + " to " + std::to_string(maxImages));
        swapchainCount = maxImages;
    }

    Log::Write(Log::Level::Info, "Swapchain length " + std::to_string(swapchainCount));
}

void Swapchain::Prepare(VkCommandBuffer buf) {
    // Convert swapchain images to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    for (uint32_t i = 0; i < swapchainCount; ++i) {
        VkImageMemoryBarrier imgBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imgBarrier.srcAccessMask = 0;  // XXX was VK_ACCESS_TRANSFER_READ_BIT wrong?
        imgBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imgBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imgBarrier.image = image[i];
        imgBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &imgBarrier);
    }
}

void Swapchain::Wait() {
    if (presentFence) {
        // Wait for the fence...
        CHECK_VKCMD(vkWaitForFences(m_vkDevice, 1, &presentFence, VK_TRUE, UINT64_MAX));
        // ...then reset the fence for future Acquire calls
        CHECK_VKCMD(vkResetFences(m_vkDevice, 1, &presentFence));
        presentFence = VK_NULL_HANDLE;
    }
}

void Swapchain::Acquire(VkSemaphore readySemaphore) {
    // If we're not using a semaphore to rate-limit the GPU, rate limit the host with a fence instead
    if (readySemaphore == VK_NULL_HANDLE) {
        Wait();
        presentFence = readyFence;
    }

    CHECK_VKCMD(vkAcquireNextImageKHR(m_vkDevice, swapchain, UINT64_MAX, readySemaphore, presentFence, &renderImageIdx));
}

void Swapchain::Present(VkQueue queue, VkSemaphore drawComplete) {
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    if (drawComplete) {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &drawComplete;
    }
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &renderImageIdx;
    auto res = vkQueuePresentKHR(queue, &presentInfo);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        Recreate();
        return;
    }
    CHECK_VKRESULT(res, "vkQueuePresentKHR");
}
#endif  // defined(USE_MIRROR_WINDOW)

struct VulkanGraphicsPlugin : public IGraphicsPlugin {
    VulkanGraphicsPlugin(const std::shared_ptr<Options>& options, std::shared_ptr<IPlatformPlugin> /*unused*/) {
        m_graphicsBinding.type = GetGraphicsBindingType();
        if (options) {
            m_noServerFramerateLock = options->NoServerFramerateLock;
            m_noFrameSkip = options->NoFrameSkip;
        }
    };

    std::vector<std::string> GetInstanceExtensions() const override { return { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME }; }

    // Note: The output must not outlive the input - this modifies the input and returns a collection of views into that modified
    // input!
    std::vector<const char*> ParseExtensionString(char* names) {
        std::vector<const char*> list;
        while (*names != 0) {
            list.push_back(names);
            while (*(++names) != 0) {
                if (*names == ' ') {
                    *names++ = '\0';
                    break;
                }
            }
        }
        return list;
    }

    static const char* const GetValidationLayerName() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        static constexpr const std::array<const char*,2> validationLayerNames{
            "VK_LAYER_KHRONOS_validation",
            "VK_LAYER_LUNARG_standard_validation"
        };

        // Enable only one validation layer from the list above. Prefer KHRONOS.
        for (const auto& validationLayerName : validationLayerNames) {
            for (const auto& layerProperties : availableLayers) {
                if (0 == strcmp(validationLayerName, layerProperties.layerName)) {
                    return validationLayerName;
                }
            }
        }

        return nullptr;
    }

    using DeviceMultiviewFeature = std::tuple<
        VkPhysicalDeviceMultiviewFeaturesKHR,
        VkPhysicalDeviceMultiviewPropertiesKHR
    >;
    DeviceMultiviewFeature GetMultiviewFeature() const
    {
        assert(m_vkPhysicalDevice != VK_NULL_HANDLE && m_vkInstance != VK_NULL_HANDLE);
#if 1
        const PFN_vkGetPhysicalDeviceFeatures2KHR fpGetPhysicalDeviceFeatures2
            = (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(m_vkInstance, "vkGetPhysicalDeviceFeatures2KHR");
        if (fpGetPhysicalDeviceFeatures2 == nullptr) {
            throw std::runtime_error(
                "Vulkan: Proc address for \"vkGetPhysicalDeviceFeatures2KHR\" not "
                "found.\n");
        }
        const PFN_vkGetPhysicalDeviceProperties2KHR fpGetPhysicalDeviceProperties2
            = (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(m_vkInstance, "vkGetPhysicalDeviceProperties2KHR");
        if (fpGetPhysicalDeviceProperties2 == nullptr) {
            throw std::runtime_error(
                "Vulkan: Proc address for \"vkGetPhysicalDeviceProperties2KHR\" not "
                "found.\n");
        }
#else
        constexpr const PFN_vkGetPhysicalDeviceFeatures2KHR fpGetPhysicalDeviceFeatures2 =
            vkGetPhysicalDeviceFeatures2KHR;
        constexpr const PFN_vkGetPhysicalDeviceProperties2 fpGetPhysicalDeviceProperties2 =
            vkGetPhysicalDeviceProperties2;
#endif
        VkPhysicalDeviceMultiviewFeaturesKHR extFeatures {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_KHR,
            .pNext = nullptr,
            .multiview = VK_FALSE,
            .multiviewGeometryShader = VK_FALSE,
            .multiviewTessellationShader = VK_FALSE
        };
        VkPhysicalDeviceFeatures2KHR deviceFeatures2 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
            .pNext = &extFeatures
        };
        fpGetPhysicalDeviceFeatures2(m_vkPhysicalDevice, &deviceFeatures2);

        VkPhysicalDeviceMultiviewPropertiesKHR extProps {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES_KHR,
            .maxMultiviewViewCount = 0,
        };
        VkPhysicalDeviceProperties2KHR deviceProps2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
            .pNext = &extProps
        };
        fpGetPhysicalDeviceProperties2(m_vkPhysicalDevice, &deviceProps2);

        return std::make_tuple(extFeatures, extProps);
    }

    void InitDeviceUUID()
    {
        CHECK(m_vkPhysicalDevice != VK_NULL_HANDLE)
#if 1
        PFN_vkGetPhysicalDeviceProperties2 fpGetPhysicalDeviceProperties2;
        fpGetPhysicalDeviceProperties2 =
            (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
                m_vkInstance, "vkGetPhysicalDeviceProperties2");
        if (fpGetPhysicalDeviceProperties2 == NULL) {
            throw std::runtime_error(
                "Vulkan: Proc address for \"vkGetPhysicalDeviceProperties2KHR\" not "
                "found.\n");
        }
#else
        constexpr const PFN_vkGetPhysicalDeviceProperties2 fpGetPhysicalDeviceProperties2 =
            vkGetPhysicalDeviceProperties2;
#endif
        VkPhysicalDeviceIDProperties vkPhysicalDeviceIDProperties {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            .pNext = nullptr
        };
        VkPhysicalDeviceProperties2 vkPhysicalDeviceProperties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &vkPhysicalDeviceIDProperties
        };
        fpGetPhysicalDeviceProperties2(m_vkPhysicalDevice, &vkPhysicalDeviceProperties2);

        std::memcpy(m_vkDeviceUUID.data(), vkPhysicalDeviceIDProperties.deviceUUID, VK_UUID_SIZE);

        if (vkPhysicalDeviceIDProperties.deviceLUIDValid)
            std::memcpy(m_vkDeviceLUID.data(), vkPhysicalDeviceIDProperties.deviceLUID, VK_UUID_SIZE);
    }

    void InitializeDevice(XrInstance instance, XrSystemId systemId, const XrEnvironmentBlendMode newMode) override {
        // Create the Vulkan device for the adapter associated with the system.
        // Extension function must be loaded by name
        XrGraphicsRequirementsVulkan2KHR graphicsRequirements{ .type=XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR, .next=nullptr };
        CHECK_XRCMD(GetVulkanGraphicsRequirements2KHR(instance, systemId, &graphicsRequirements));

        std::vector<const char*> layers;
#ifdef XR_ENABLE_VULKAN_VALIDATION_LAYER
        if (const char* const validationLayerName = GetValidationLayerName()) {
            layers.push_back(validationLayerName);
        }
        else {
            Log::Write(Log::Level::Warning, "No validation layers found in the system, skipping");
        }
#endif

        const std::vector<const char*> extensions =
        {
            "VK_EXT_debug_report",
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
#if defined(USE_MIRROR_WINDOW)
            "VK_KHR_surface",
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            "VK_KHR_win32_surface",
#else
#error CreateSurface not supported on this OS
#endif  // defined(VK_USE_PLATFORM_WIN32_KHR)
#endif  // defined(USE_MIRROR_WINDOW)
        };

        VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "alxr-client",
            .applicationVersion = 1,
            .pEngineName = "alxr-engine",
            .engineVersion = 1,
        };
#ifdef XR_USE_PLATFORM_ANDROID
#pragma message ("Using Vulkan API version 1.1")
        appInfo.apiVersion = VK_API_VERSION_1_1;
        const char* const vulkanVersionStr = "1.1";
#else
#if defined(VK_API_VERSION_1_3) && (VK_VERSION_1_3 > 0)
#pragma message ("Using Vulkan API version 1.3")
        appInfo.apiVersion = VK_API_VERSION_1_3;
        const char* const vulkanVersionStr = "1.3";
#elif defined(VK_API_VERSION_1_2) && (VK_VERSION_1_2 > 0)
#pragma message ("Using Vulkan API version 1.2")
        appInfo.apiVersion = VK_API_VERSION_1_2;
        const char* const vulkanVersionStr = "1.2";
#elif defined(VK_API_VERSION_1_1) && (VK_VERSION_1_1 > 0)
#pragma message ("Using Vulkan API version 1.1")
        appInfo.apiVersion = VK_API_VERSION_1_1;
        const char* const vulkanVersionStr = "1.1";
#else
#error "Vulkan versions below 1.1 are not supported!"
#endif
#endif
        Log::Write(Log::Level::Info, Fmt("Using Vulkan version: %s", vulkanVersionStr));

        const VkInstanceCreateInfo instInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = (uint32_t)layers.size(),
            .ppEnabledLayerNames = layers.empty() ? nullptr : layers.data(),
            .enabledExtensionCount = (uint32_t)extensions.size(),
            .ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data()
        };
        const XrVulkanInstanceCreateInfoKHR createInfo{
            .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
            .next = nullptr,
            .systemId = systemId,
            .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
            .vulkanCreateInfo = &instInfo,
            .vulkanAllocator = nullptr
        };
        VkResult err{};
        CHECK_XRCMD(CreateVulkanInstanceKHR(instance, &createInfo, &m_vkInstance, &err));
        CHECK_VKCMD(err);

        vkCreateDebugReportCallbackEXT =
            (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(m_vkInstance, "vkCreateDebugReportCallbackEXT");
        vkDestroyDebugReportCallbackEXT =
            (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(m_vkInstance, "vkDestroyDebugReportCallbackEXT");

        const VkDebugReportCallbackCreateInfoEXT debugInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
            .pNext = nullptr,
#ifdef XR_ENABLE_VULKAN_VALIDATION_LAYER
            .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT,
#else
            .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT,
#endif
            .pfnCallback = debugReportThunk,
            .pUserData = this
        };
        CHECK_VKCMD(vkCreateDebugReportCallbackEXT(m_vkInstance, &debugInfo, nullptr, &m_vkDebugReporter));

        const XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{
            .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
            .next = nullptr,
            .systemId = systemId,
            .vulkanInstance = m_vkInstance
        };
        CHECK_XRCMD(GetVulkanGraphicsDevice2KHR(instance, &deviceGetInfo, &m_vkPhysicalDevice));

        InitDeviceUUID();

        const std::array<const float, 2> queuePriorities = { 1.0f, 0.0f };
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilyProps(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, &queueFamilyProps[0]);

        VkDeviceQueueCreateInfo queueInfo[2] {
            {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .queueCount = static_cast<std::uint32_t>(queuePriorities.size()),
                .pQueuePriorities = queuePriorities.data()
            },
            {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .queueCount = 1,
                .pQueuePriorities = queuePriorities.data()+1
            },
        };
 
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            // Only need graphics (not presentation) for draw queue
            if ((queueFamilyProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                m_queueFamilyIndex = m_queueFamilyIndexVideoCpy = queueInfo[0].queueFamilyIndex = i;
                break;
            }
        }

        std::uint32_t queueInfoCount = 1, cpyQueueIndex = 1;;
        if (queueFamilyProps[m_queueFamilyIndex].queueCount < 2) // no free queue graphics family, find free queue with transfer flag
        {
            for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                // Only need transfer (not graphics) for video queue
                if ( i != m_queueFamilyIndex && (queueFamilyProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0u) {
                    m_queueFamilyIndexVideoCpy = queueInfo[1].queueFamilyIndex = i;
                    break;
                }
            }
            queueInfoCount = 2, cpyQueueIndex = 0, queueInfo[0].queueCount = 1;
        }

        std::vector<const char*> deviceExtensions =
        {
#if defined(USE_MIRROR_WINDOW)
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#endif
            //VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            //VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            //VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#ifdef XR_USE_PLATFORM_ANDROID
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
#else
    #ifdef _WIN64
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    #else
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    #endif
            //VK_KHR_timeline_semaphore
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#endif
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME
        };

        const auto [multiviewFeature, multiviewProps] = GetMultiviewFeature();
        if (multiviewFeature.multiview && multiviewProps.maxMultiviewViewCount > 1) {
            m_isMultiViewSupported = true;
            deviceExtensions.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);            
            Log::Write(Log::Level::Verbose, Fmt(
                "VulkanGraphicsPlugin: Multiview features:\n"
                "\tmultiview: %s\n"
                "\tmultiviewGeometryShader: %s\n"
                "\tmultiviewTessellationShader: %s",
                multiviewFeature.multiview ? "true" : "false",
                multiviewFeature.multiviewGeometryShader ? "true" : "false",
                multiviewFeature.multiviewTessellationShader ? "true" : "false"));
            Log::Write(Log::Level::Verbose, Fmt(
                "VulkanGraphicsPlugin: Multiview properties:\n"
                "\tmaxMultiviewViewCount: %d\n"
                "\tmaxMultiviewInstanceIndex: %d",
                multiviewProps.maxMultiviewViewCount,
                multiviewProps.maxMultiviewInstanceIndex));
        }

        VkPhysicalDeviceFeatures features{};
        // features.samplerAnisotropy = VK_TRUE;
        VkPhysicalDeviceVulkan11Features features11 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = nullptr,
            .multiview = m_isMultiViewSupported ? VK_TRUE : VK_FALSE,
            .samplerYcbcrConversion = VK_TRUE,
        };
        const VkPhysicalDeviceFeatures2 features2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &features11,
            .features = features            
        };
        const VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features2,
            .queueCreateInfoCount = queueInfoCount,
            .pQueueCreateInfos = &queueInfo[0],
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = (uint32_t)deviceExtensions.size(),
            .ppEnabledExtensionNames = deviceExtensions.empty() ? nullptr : deviceExtensions.data(),
            .pEnabledFeatures = nullptr
        };
        const XrVulkanDeviceCreateInfoKHR deviceCreateInfo {
            .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
            .next = nullptr,
            .systemId = systemId,
            .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
            .vulkanPhysicalDevice = m_vkPhysicalDevice,
            .vulkanCreateInfo = &deviceInfo,            
            .vulkanAllocator = nullptr,
        };
        CHECK_XRCMD(CreateVulkanDeviceKHR(instance, &deviceCreateInfo, &m_vkDevice, &err));
        CHECK_VKCMD(err);

        vkGetDeviceQueue(m_vkDevice, queueInfo[0].queueFamilyIndex, 0, &m_vkQueue);
        vkGetDeviceQueue(m_vkDevice, m_queueFamilyIndexVideoCpy, cpyQueueIndex, &m_VideoCpyQueue);
        CHECK(m_VideoCpyQueue != VK_NULL_HANDLE);

        m_memAllocator.Init(m_vkPhysicalDevice, m_vkDevice);

        InitializeResources();

        m_graphicsBinding.instance = m_vkInstance;
        m_graphicsBinding.physicalDevice = m_vkPhysicalDevice;
        m_graphicsBinding.device = m_vkDevice;
        m_graphicsBinding.queueFamilyIndex = queueInfo[0].queueFamilyIndex;
        m_graphicsBinding.queueIndex = 0;

        SetEnvironmentBlendMode(newMode);
    }

#ifdef USE_ONLINE_VULKAN_SHADERC
    // Compile a shader to a SPIR-V binary.
    std::vector<uint32_t> CompileGlslShader(const std::string& name, shaderc_shader_kind kind, const std::string& source) {
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        options.SetOptimizationLevel(shaderc_optimization_level_size);

        shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source, kind, name.c_str(), options);

        if (module.GetCompilationStatus() != shaderc_compilation_status_success) {
            Log::Write(Log::Level::Error, Fmt("Shader %s compilation failed: %s", name.c_str(), module.GetErrorMessage().c_str()));
            return std::vector<uint32_t>();
        }

        return { module.cbegin(), module.cend() };
    }
#endif

    void InitializeD3D11VA()
    {
#if defined(XR_USE_GRAPHICS_API_D3D11)
        using namespace Microsoft::WRL;
        using namespace DirectX;
        const LUID adapterLUID = *reinterpret_cast<const LUID*>(m_vkDeviceLUID.data());
        const ComPtr<IDXGIAdapter1> adapter = ALXR::GetAdapter(adapterLUID);
        if (adapter == nullptr) {
            Log::Write(Log::Level::Warning, "Failed to find suitable adaptor, client will fallback to an unknown device type.");
        }
        using FeatureLvlList = std::vector<D3D_FEATURE_LEVEL>;
        const FeatureLvlList featureLevels =
        {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        UINT creationFlags = /*D3D11_CREATE_DEVICE_BGRA_SUPPORT |*/ D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if !defined(NDEBUG)
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        // Create the Direct3D 11 API device object and a corresponding context.
        D3D_DRIVER_TYPE driverType = ((adapter == nullptr) ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN);
        Log::Write(Log::Level::Verbose, Fmt("Selected driver type: %d", static_cast<int>(driverType)));

    TryAgain:
        const HRESULT hr = D3D11CreateDevice
        (
            adapter.Get(), driverType, 0, creationFlags,
            featureLevels.data(), (UINT)featureLevels.size(),
            D3D11_SDK_VERSION, m_d3d11vaDevice.ReleaseAndGetAddressOf(),
            nullptr, nullptr//m_d3d11vaDeviceCtx.ReleaseAndGetAddressOf()
        );
        if (FAILED(hr)) {
            // If initialization failed, it may be because device debugging isn't supported, so retry without that.
            if ((creationFlags & D3D11_CREATE_DEVICE_DEBUG) && (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)) {
                creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
                goto TryAgain;
            }

            // If the initialization still fails, fall back to the WARP device.
            // For more information on WARP, see: http://go.microsoft.com/fwlink/?LinkId=286690
            if (driverType != D3D_DRIVER_TYPE_WARP) {
                driverType = D3D_DRIVER_TYPE_WARP;
                goto TryAgain;
            }
        }

        CHECK(m_d3d11vaDevice != nullptr);
        ID3D10Multithread* pMultithread = nullptr;
        if (SUCCEEDED(m_d3d11vaDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)&pMultithread))
            && pMultithread != nullptr) {
            pMultithread->SetMultithreadProtected(TRUE);
            pMultithread->Release();
        }
#endif
    }

#if defined(XR_USE_GRAPHICS_API_D3D11)
    virtual const void* GetD3D11AVDevice() const { return m_d3d11vaDevice.Get(); }
    virtual void* GetD3D11AVDevice() { return m_d3d11vaDevice.Get(); }

    virtual const void* GetD3D11VADeviceContext() const { return nullptr; }// return m_d3d11vaDeviceCtx.Get(); }
    virtual void* GetD3D11VADeviceContext() { return nullptr; }//return m_d3d11vaDeviceCtx.Get(); }
#endif

    enum /*class*/ VideoFragShaderType : std::size_t {
        Normal,
        FoveatedDecode,
        TypeCount
    };

    void InitializeVideoResources() 
    {
        InitializeD3D11VA();
#ifndef XR_USE_PLATFORM_ANDROID
        m_texRendereComplete.Create(m_vkDevice, true);
        m_texCopy.Create(m_vkDevice, true);
#endif
#ifdef XR_ENABLE_CUDA_INTEROP
        InitCuda();
#endif

        using CodeBufferList = std::array<CodeBuffer, size_t(PassthroughMode::TypeCount)>;
        using CodeBufferMap  = std::array<CodeBufferList, VideoFragShaderType::TypeCount>;

        CodeBuffer vertexShader;
        CodeBufferMap fragShaders;
        if (IsMultiViewEnabled()) {
            vertexShader =
                SPV_PREFIX
                    #include "shaders/multiview/videoStream_vert.spv"
                SPV_SUFFIX;
            fragShaders[VideoFragShaderType::Normal] = {{
                SPV_PREFIX
                    #include "shaders/multiview/videoStream_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/multiview/passthroughBlend_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/multiview/passthroughMask_frag.spv"
                SPV_SUFFIX
            } };
            fragShaders[VideoFragShaderType::FoveatedDecode] = {{
                SPV_PREFIX
                    #include "shaders/multiview/fovDecode/videoStream_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/multiview/fovDecode/passthroughBlend_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/multiview/fovDecode/passthroughMask_frag.spv"
                SPV_SUFFIX
            }};
        }
        else {
            vertexShader =
                SPV_PREFIX
                    #include "shaders/videoStream_vert.spv"
                SPV_SUFFIX;
            fragShaders[VideoFragShaderType::Normal] = {{
                SPV_PREFIX
                    #include "shaders/videoStream_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/passthroughBlend_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/passthroughMask_frag.spv"
                SPV_SUFFIX
            }};
            fragShaders[VideoFragShaderType::FoveatedDecode] = { {
                SPV_PREFIX
                    #include "shaders/fovDecode/videoStream_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/fovDecode/passthroughBlend_frag.spv"
                SPV_SUFFIX,
                SPV_PREFIX
                    #include "shaders/fovDecode/passthroughMask_frag.spv"
                SPV_SUFFIX
            }};
        }

        for (const auto shaderType : { VideoFragShaderType::Normal,
                                       VideoFragShaderType::FoveatedDecode }) {
            
            const auto& fragList = fragShaders[shaderType];
            auto& vsList = m_videoShaders[shaderType];
            assert(fragList.size() == vsList.size());

            for (std::size_t index = 0; index < fragList.size(); ++index) {
                const auto& fragShader = fragList[index];
                CHECK(fragShader.size() > 0);
                auto& vidShader = vsList[index];
                vidShader.Init(m_vkDevice);
                vidShader.LoadVertexShader(vertexShader);
                vidShader.LoadFragmentShader(fragShader);
            }
        }

        if (!m_videoCpyCmdBuffer.Init(m_vkDevice, m_queueFamilyIndexVideoCpy)) THROW("Failed to create command buffer");

        m_quadBuffer.Init(m_vkDevice, &m_memAllocator,
            { {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Geometry::QuadVertex, position)},
             {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Geometry::QuadVertex, uv)} });
        const std::uint32_t quadIndexCount = sizeof(Geometry::QuadIndices) / sizeof(Geometry::QuadIndices[0]);
        const std::uint32_t quadVertexCount = sizeof(Geometry::QuadVertices) / sizeof(Geometry::QuadVertices[0]);
        m_quadBuffer.Create(quadIndexCount, quadVertexCount);
        m_quadBuffer.UpdateIndices(Geometry::QuadIndices.data(), quadIndexCount, 0);
        m_quadBuffer.UpdateVertices(Geometry::QuadVertices.data(), quadVertexCount, 0);
    }

    using CodeBuffer = ShaderProgram::CodeBuffer;

    void InitializeResources() {
#ifdef USE_ONLINE_VULKAN_SHADERC
        auto vertexSPIRV = CompileGlslShader("vertex", shaderc_glsl_default_vertex_shader, VertexShaderGlsl);
        auto fragmentSPIRV = CompileGlslShader("fragment", shaderc_glsl_default_fragment_shader, FragmentShaderGlsl);
#else

        CodeBuffer vertexSPIRV, fragmentSPIRV;
        if (IsMultiViewEnabled()) {
            vertexSPIRV =
                SPV_PREFIX
                    #include "shaders/multiview/lobby_vert.spv"
                SPV_SUFFIX;            
            fragmentSPIRV =
                SPV_PREFIX
                    #include "shaders/multiview/lobby_frag.spv"
                SPV_SUFFIX;
        }
        else {
            vertexSPIRV =
                SPV_PREFIX
                    #include "shaders/lobby_vert.spv"
                SPV_SUFFIX;            
            fragmentSPIRV =
                SPV_PREFIX
                    #include "shaders/lobby_frag.spv"
                SPV_SUFFIX;
        }
#endif

        if (vertexSPIRV.empty()) THROW("Failed to compile vertex shader");
        if (fragmentSPIRV.empty()) THROW("Failed to compile fragment shader");

        m_shaderProgram.Init(m_vkDevice);
        m_shaderProgram.LoadVertexShader(vertexSPIRV);
        m_shaderProgram.LoadFragmentShader(fragmentSPIRV);

        if (!m_cmdBuffer.Init(m_vkDevice, m_queueFamilyIndex)) THROW("Failed to create command buffer");

        m_pipelineLayout.Create(m_vkDevice, m_vkInstance, m_isMultiViewSupported);

        static_assert(sizeof(Geometry::Vertex) == 24, "Unexpected Vertex size");
        m_drawBuffer.Init(m_vkDevice, &m_memAllocator,
            { {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Geometry::Vertex, Position)},
             {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Geometry::Vertex, Color)} });
        uint32_t numCubeIdicies = sizeof(Geometry::c_cubeIndices) / sizeof(Geometry::c_cubeIndices[0]);
        uint32_t numCubeVerticies = sizeof(Geometry::c_cubeVertices) / sizeof(Geometry::c_cubeVertices[0]);
        m_drawBuffer.Create(numCubeIdicies, numCubeVerticies);
        m_drawBuffer.UpdateIndices(Geometry::c_cubeIndices, numCubeIdicies, 0);
        m_drawBuffer.UpdateVertices(Geometry::c_cubeVertices, numCubeVerticies, 0);

        InitializeVideoResources();

#if defined(USE_MIRROR_WINDOW)
        m_swapchain.Create(m_vkInstance, m_vkPhysicalDevice, m_vkDevice, m_graphicsBinding.queueFamilyIndex);

        m_cmdBuffer.Reset();
        m_cmdBuffer.Begin();
        m_swapchain.Prepare(m_cmdBuffer.buf);
        m_cmdBuffer.End();
        m_cmdBuffer.Exec(m_vkQueue);
        m_cmdBuffer.Wait();
#endif
    }

    int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        // List of supported color swapchain formats.
        constexpr const int64_t SupportedColorSwapchainFormats[] = { VK_FORMAT_B8G8R8A8_SRGB , VK_FORMAT_R8G8B8A8_SRGB,
                                                                     VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };

        auto swapchainFormatIt =
            std::find_first_of(runtimeFormats.begin(), runtimeFormats.end(), std::begin(SupportedColorSwapchainFormats),
                std::end(SupportedColorSwapchainFormats));
        if (swapchainFormatIt == runtimeFormats.end()) {
            THROW("No runtime swapchain format supported for color swapchain");
        }

        return *swapchainFormatIt;
    }

    const XrBaseInStructure* GetGraphicsBinding() const override {
        return reinterpret_cast<const XrBaseInStructure*>(&m_graphicsBinding);
    }

    std::vector<XrSwapchainImageBaseHeader*> AllocateSwapchainImageStructs(
        uint32_t capacity, const XrSwapchainCreateInfo& swapchainCreateInfo) override {
        // Allocate and initialize the buffer of image structs (must be sequential in memory for xrEnumerateSwapchainImages).
        // Return back an array of pointers to each swapchain image struct so the consumer doesn't need to know the type/size.
        // Keep the buffer alive by adding it into the list of buffers.
        m_swapchainImageContexts.emplace_back(GetSwapchainImageType());
        SwapchainImageContext& swapchainImageContext = m_swapchainImageContexts.back();

        std::vector<XrSwapchainImageBaseHeader*> bases = swapchainImageContext.Create(
            m_vkDevice, &m_memAllocator, capacity, swapchainCreateInfo, m_pipelineLayout, m_shaderProgram, m_drawBuffer);

        // Map every swapchainImage base pointer to this context
        for (auto& base : bases) {
            m_swapchainImageContextMap[base] = &swapchainImageContext;
        }

        return bases;
    }

    virtual void ClearSwapchainImageStructs() override
    {
        m_swapchainImageContextMap.clear();
        m_swapchainImageContexts.clear();
    }

    static inline void MakeViewProjMatrix(XrMatrix4x4f& vp, const XrCompositionLayerProjectionView& layerView) {
        const auto& pose = layerView.pose;
        XrMatrix4x4f proj;
        XrMatrix4x4f_CreateProjectionFov(&proj, GRAPHICS_VULKAN, layerView.fov, 0.05f, 100.0f);
        XrMatrix4x4f toView;
        constexpr static const XrVector3f scale{ 1.f, 1.f, 1.f };
        XrMatrix4x4f_CreateTranslationRotationScale(&toView, &pose.position, &pose.orientation, &scale);
        XrMatrix4x4f view;
        XrMatrix4x4f_InvertRigidBody(&view, &toView);
        //XrMatrix4x4f vp;
        XrMatrix4x4f_Multiply(&vp, &proj, &view);
    }

    template < typename RenderFunc >
    inline void RenderViewImpl(const XrSwapchainImageBaseHeader* swapchainImage, RenderFunc&& renderFun) {

        const auto swapchainContextPtr = m_swapchainImageContextMap[swapchainImage];
        assert(swapchainContextPtr != nullptr);
        const std::uint32_t imageIndex = swapchainContextPtr->ImageIndex(swapchainImage);

        if (m_cmdBufferWaitNextFrame) {
            m_cmdBuffer.Wait();
        }
        m_cmdBuffer.Reset();
#ifdef XR_USE_PLATFORM_ANDROID
        m_videoTextures[VidTextureIndex::DeferredDelete].Clear();
#endif
        m_cmdBuffer.Begin();

        // Ensure depth is in the right layout
        swapchainContextPtr->depthBuffer.TransitionLayout(&m_cmdBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        renderFun(imageIndex, *swapchainContextPtr);

        m_cmdBuffer.End();
#if 1 //#ifdef XR_USE_PLATFORM_ANDROID
        m_cmdBuffer.Exec(m_vkQueue);
#else
        m_cmdBuffer.Exec<1, 1>(m_vkQueue, { &m_texRendereComplete }, { &m_texCopy }, { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT });
#endif
        if (!m_cmdBufferWaitNextFrame) {
            m_cmdBuffer.Wait();
        }

#if defined(USE_MIRROR_WINDOW)
        // Cycle the window's swapchain on the last view rendered
        if (swapchainContext == &m_swapchainImageContexts.back()) {
            m_swapchain.Acquire();
            m_swapchain.Present(m_vkQueue);
        }
#endif
    }

    using ClearValueT = std::array<const VkClearValue, 2>;
    using OpaqueClear = ClearValueT;
    using AdditiveClear = ClearValueT;
    using AlphaBlendClear = ClearValueT;
    using CColorType = std::array<const float, 3>;
    constexpr static const CColorType DarkGraySlate{ 0.184313729f, 0.309803933f, 0.309803933f };
    constexpr static const CColorType CClear { 0.0f, 0.0f, 0.0f };

    constexpr static const VkClearValue ClearDepthStencilValue{
        .depthStencil {
            .depth = 1.0f,
            .stencil = 0
        }
    };
    constexpr static const std::array<const ClearValueT, 4> ConstClearValues{
        OpaqueClear {
            VkClearValue {.color {.float32 = { DarkGraySlate[0], DarkGraySlate[1], DarkGraySlate[2], 1.0f}}},
            ClearDepthStencilValue
        },
        AdditiveClear {
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 0.0f}}},
            ClearDepthStencilValue
        },
        AlphaBlendClear {
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 0.5f }}},
            ClearDepthStencilValue
        },
        OpaqueClear { // for XR_FB_passthrough / Passthrough Modes.
            VkClearValue {.color {.float32 = { DarkGraySlate[0], DarkGraySlate[1], DarkGraySlate[2], 0.2f}}},
            ClearDepthStencilValue
        },
    };
    static_assert(ConstClearValues.size() >= 4);

    constexpr static const std::array<const ClearValueT, 4> VideoClearValues{
        OpaqueClear {
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 1.0f }}},
            ClearDepthStencilValue
        },
        AdditiveClear {
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 0.0f }}},
            ClearDepthStencilValue
        },
        AlphaBlendClear {
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 0.5f }}},
            ClearDepthStencilValue
        },
        OpaqueClear { // for XR_FB_passthrough / Passthrough Modes.
            VkClearValue {.color {.float32 = { CClear[0], CClear[1], CClear[2], 0.2f}}},
            ClearDepthStencilValue
        },
    };
    static_assert(VideoClearValues.size() >= 4);

    inline std::size_t ClearValueIndex(const PassthroughMode ptMode) const {       
        return ptMode == PassthroughMode::None ? m_clearColorIndex : 3u;
    }

    void RenderMultiView
    (
        const std::array<XrCompositionLayerProjectionView, 2>& layerViews,
        const XrSwapchainImageBaseHeader* swapchainImage,
        const std::int64_t /*swapchainFormat*/,
        const PassthroughMode newMode,
        const std::vector<Cube>& cubes
    ) override {
        assert(m_isMultiViewSupported);
        RenderViewImpl(swapchainImage, [&, this](const std::uint32_t imageIndex, auto& swapchainContext)
        {
            const auto& clearValues = ConstClearValues[ClearValueIndex(newMode)];
            VkRenderPassBeginInfo renderPassBeginInfo{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .pNext = nullptr,
                .clearValueCount = (uint32_t)clearValues.size(),
                .pClearValues = clearValues.data()
            };
            // Bind and clear eye render target
            swapchainContext.BindRenderTarget(imageIndex, /*out*/ renderPassBeginInfo);

            vkCmdBeginRenderPass(m_cmdBuffer.buf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, swapchainContext.pipe.pipe);

            // Bind index and vertex buffers
            vkCmdBindIndexBuffer(m_cmdBuffer.buf, m_drawBuffer.idxBuf, 0, VK_INDEX_TYPE_UINT16);
            constexpr const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(m_cmdBuffer.buf, 0, 1, &m_drawBuffer.vtxBuf, &offset);

            // Compute the view-projection transform.
            // Note all matrixes (including OpenXR's) are column-major, right-handed.
            std::array<XrMatrix4x4f,2> vps{};
            for (std::size_t viewIndex = 0; viewIndex < layerViews.size(); ++viewIndex) {
                MakeViewProjMatrix(vps[viewIndex], layerViews[viewIndex]);
            }

            // Render each cube
            for (const Cube& cube : cubes) {
                // Compute the model-view-projection transform and push it.
                MultiViewProjectionUniform mvps;
                for (std::size_t viewIndex = 0; viewIndex < layerViews.size(); ++viewIndex) {
                    XrMatrix4x4f model;
                    XrMatrix4x4f_CreateTranslationRotationScale(&model, &cube.Pose.position, &cube.Pose.orientation, &cube.Scale);
                    XrMatrix4x4f_Multiply(&mvps.mvp[viewIndex], &vps[viewIndex], &model);
                }
                vkCmdPushConstants(m_cmdBuffer.buf, m_pipelineLayout.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MultiViewProjectionUniform), &mvps);

                // Draw the cube.
                vkCmdDrawIndexed(m_cmdBuffer.buf, m_drawBuffer.count.idx, 1, 0, 0, 0);
            }

            vkCmdEndRenderPass(m_cmdBuffer.buf);
        });
    }

    void RenderView
    (
        const XrCompositionLayerProjectionView& layerView, const XrSwapchainImageBaseHeader* swapchainImage,
        const std::int64_t /*swapchainFormat*/, const PassthroughMode newMode,
        const std::vector<Cube>& cubes
    ) override {
        assert(layerView.subImage.imageArrayIndex == 0);  // Texture arrays not supported.
        RenderViewImpl(swapchainImage, [&, this](const std::uint32_t imageIndex, auto& swapchainContext)
        {
            const auto& clearValues = ConstClearValues[ClearValueIndex(newMode)];
            VkRenderPassBeginInfo renderPassBeginInfo{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .pNext = nullptr,
                .clearValueCount = (uint32_t)clearValues.size(),
                .pClearValues = clearValues.data()
            };
            // Bind and clear eye render target
            swapchainContext.BindRenderTarget(imageIndex, /*out*/ renderPassBeginInfo);

            vkCmdBeginRenderPass(m_cmdBuffer.buf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, swapchainContext.pipe.pipe);

            // Bind index and vertex buffers
            vkCmdBindIndexBuffer(m_cmdBuffer.buf, m_drawBuffer.idxBuf, 0, VK_INDEX_TYPE_UINT16);
            constexpr const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(m_cmdBuffer.buf, 0, 1, &m_drawBuffer.vtxBuf, &offset);

            // Compute the view-projection transform.
            // Note all matrixes (including OpenXR's) are column-major, right-handed.
            XrMatrix4x4f vp;
            MakeViewProjMatrix(vp, layerView);

            // Render each cube
            for (const Cube& cube : cubes) {
                // Compute the model-view-projection transform and push it.
                XrMatrix4x4f model;
                XrMatrix4x4f_CreateTranslationRotationScale(&model, &cube.Pose.position, &cube.Pose.orientation, &cube.Scale);
                XrMatrix4x4f mvp;
                XrMatrix4x4f_Multiply(&mvp, &vp, &model);
                vkCmdPushConstants(m_cmdBuffer.buf, m_pipelineLayout.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp.m), &mvp.m[0]);

                // Draw the cube.
                vkCmdDrawIndexed(m_cmdBuffer.buf, m_drawBuffer.count.idx, 1, 0, 0, 0);
            }

            vkCmdEndRenderPass(m_cmdBuffer.buf);
        });
    }

    uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override { return VK_SAMPLE_COUNT_1_BIT; }

    constexpr static VkFormat MapFormat(const XrPixelFormat pixfmt)
    {
        switch (pixfmt)
        {
        case XrPixelFormat::G8_B8_R8_3PLANE_420:
            return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
        case XrPixelFormat::G10X6_B10X6_R10X6_3PLANE_420:
            return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
        case XrPixelFormat::NV12:
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case XrPixelFormat::P010LE:
            return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        default:return VK_FORMAT_UNDEFINED;
        }
    }

    constexpr static inline VkFormat GetLumaFormat(const VkFormat pixfmt)
    {
        switch (pixfmt)
        {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: // TODO: Change this how this works!
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: // TODO: Change this how this works!
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return VK_FORMAT_R10X6_UNORM_PACK16;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    constexpr static inline VkFormat GetChromaFormat(const VkFormat pixfmt)
    {
        switch (pixfmt)
        {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: // TODO: Change this how this works!
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: // TODO: Change this how this works!
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    constexpr static inline std::size_t FormatSize(const VkFormat pixfmt)
    {
        switch (pixfmt)
        {
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_UNORM: return sizeof(std::uint8_t);
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_UNORM: return sizeof(std::uint8_t) * 2;
        case VK_FORMAT_R10X6_UNORM_PACK16: return sizeof(std::uint16_t);
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16: return sizeof(std::uint16_t) * 2;
        default: return 0;
        }
    }

    constexpr static inline std::size_t LumaSize(const VkFormat pixfmt)
    {
        return FormatSize(GetLumaFormat(pixfmt));
    }

    constexpr static inline std::size_t ChromaSize(const VkFormat pixfmt)
    {
        return FormatSize(GetChromaFormat(pixfmt));
    }

    constexpr static inline std::size_t StagingBufferSize(const std::size_t w, const std::size_t h, const VkFormat format)
    {
        return (w * h * LumaSize(format)) + (((w * h) / 2) * ChromaSize(format));
    }

    VkDeviceSize createStaggingBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory)
    {
        const VkBufferCreateInfo bufferInfo {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK_VKCMD(vkCreateBuffer(m_vkDevice, &bufferInfo, nullptr, &buffer));

        VkMemoryRequirements memRequirements{};
        vkGetBufferMemoryRequirements(m_vkDevice, buffer, &memRequirements);
        m_memAllocator.Allocate(memRequirements, &bufferMemory, properties);

        CHECK_VKCMD(vkBindBufferMemory(m_vkDevice, buffer, bufferMemory, 0));

        return memRequirements.size;
    }

    void ClearImageDescriptorSetLayouts()
    {
        if (m_vkDevice == VK_NULL_HANDLE)
            return;
        if (m_descriptorSets.size() > 0) {
            assert(m_descriptorPool != VK_NULL_HANDLE);
            CHECK_VKCMD(vkFreeDescriptorSets(m_vkDevice, m_descriptorPool, (std::uint32_t)m_descriptorSets.size(), m_descriptorSets.data()));
        }
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_vkDevice, m_descriptorPool, nullptr);
        }
        m_descriptorSets.clear();
        m_descriptorPool = VK_NULL_HANDLE;
    }

    void CreateImageDescriptorSetLayouts()
    {
        if (m_descriptorPool != VK_NULL_HANDLE ||
            m_vkDevice == VK_NULL_HANDLE)
            return;
        const std::uint32_t swapChainCount = static_cast<uint32_t>(m_swapchainImageContexts.back().swapchainImages.size());
        
        const std::array<const VkDescriptorPoolSize, 1> poolSizes{
            VkDescriptorPoolSize {
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = swapChainCount,
            }
        };
        const VkDescriptorPoolCreateInfo poolInfo {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .maxSets = swapChainCount,
            .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data()
        };
        CHECK_VKCMD(vkCreateDescriptorPool(m_vkDevice, &poolInfo, nullptr, &m_descriptorPool));
        CHECK(m_descriptorPool != VK_NULL_HANDLE);

        const VkDescriptorSetAllocateInfo allocInfo {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_videoStreamLayout.descriptorSetLayout
        };
        m_descriptorSets.resize(1);
        CHECK_VKCMD(vkAllocateDescriptorSets(m_vkDevice, &allocInfo, m_descriptorSets.data()));
    }

    struct alignas(16) SpecializationData {
        ALXR::FoveatedDecodeParams fdParams;
        VkBool32 enableSRGBLinearize;
        float alphaValue;     // Blend or Mask mode.
        XrVector3f keyColour; // Mask Mode only.
    };
    using SpecializationMap = std::vector<VkSpecializationMapEntry>;
    SpecializationMap MakeSpecializationMap
    (
        const bool enableFDParams,
        const PassthroughMode ptMode
    ) const
    {
        using SDType = SpecializationData;
        static_assert(std::is_standard_layout<SDType>::value);
        constexpr static const std::array<std::uint32_t, 13> MemberOffsets {
            offsetof(SDType, fdParams.eyeSizeRatio.x),
            offsetof(SDType, fdParams.eyeSizeRatio.y),
            offsetof(SDType, fdParams.centerSize.x),
            offsetof(SDType, fdParams.centerSize.y),
            offsetof(SDType, fdParams.centerShift.x),
            offsetof(SDType, fdParams.centerShift.y),
            offsetof(SDType, fdParams.edgeRatio.x),
            offsetof(SDType, fdParams.edgeRatio.y),
            offsetof(SDType, enableSRGBLinearize),
            offsetof(SDType, alphaValue),
            offsetof(SDType, keyColour.x),
            offsetof(SDType, keyColour.y),
            offsetof(SDType, keyColour.z),
        };
        std::uint32_t memberOffsetPos = 0;
        SpecializationMap specializationEMap{};
        specializationEMap.reserve(MemberOffsets.size());
        if (enableFDParams) {
            for (; memberOffsetPos < 8u; ++memberOffsetPos) {
                assert(memberOffsetPos < MemberOffsets.size());
                specializationEMap.push_back({
                    .constantID = memberOffsetPos,
                    .offset = MemberOffsets[memberOffsetPos],
                    .size = sizeof(float)
                    });
            }
        } else {
            memberOffsetPos = 8;
        }

        const std::uint32_t constID = memberOffsetPos++;
        assert(memberOffsetPos < MemberOffsets.size());
        specializationEMap.push_back({
            .constantID = constID,
            .offset = MemberOffsets[constID],
            .size = sizeof(VkBool32)
        });

        switch (ptMode) {
            case PassthroughMode::MaskLayer:
            case PassthroughMode::BlendLayer:
            {
                const std::uint32_t constID = memberOffsetPos++;
                assert(memberOffsetPos < MemberOffsets.size());
                specializationEMap.push_back({
                    .constantID = constID,
                    .offset = MemberOffsets[constID],
                    .size = sizeof(float)
                });
                break;
            }
        }

        if (ptMode == PassthroughMode::MaskLayer) {
            for (; memberOffsetPos < MemberOffsets.size(); ++memberOffsetPos) {
                specializationEMap.push_back({
                    .constantID = memberOffsetPos,
                    .offset = MemberOffsets[memberOffsetPos],
                    .size = sizeof(float)
                });
            }
        }

        return specializationEMap;
    }

    void CreateVideoStreamPipeline(const VkSamplerYcbcrConversionCreateInfo& conversionInfo)
    {
        //ClearVideoTextures();
        /////////////////////////
        assert(m_videoStreamLayout.IsNull());
        m_videoStreamLayout.CreateVideoStreamLayout(conversionInfo, m_vkDevice, m_vkInstance, m_isMultiViewSupported);
                
        const auto fovDecodeParamPtr = m_fovDecodeParams;
        const auto shaderType = fovDecodeParamPtr ?
            VideoFragShaderType::FoveatedDecode :
            VideoFragShaderType::Normal;

        CHECK(m_swapchainImageContexts.size() > 0);
        const auto& swapChainInfo = m_swapchainImageContexts.back();
        std::size_t pipelineIdx = 0;
        auto& shaderList = m_videoShaders[shaderType];
        assert(shaderList.size() <= m_videoStreamPipelines.size());
        for (std::size_t videoShaderIdx = 0; videoShaderIdx < shaderList.size(); ++videoShaderIdx) {
            auto& videoShader = shaderList[videoShaderIdx];
            auto& fragShaderInfo = videoShader.shaderInfo[1];

            const auto passthroughMode = static_cast<PassthroughMode>(videoShaderIdx);
            const SpecializationData specializationConst{
                .fdParams = fovDecodeParamPtr ? *fovDecodeParamPtr : ALXR::FoveatedDecodeParams{},
                .enableSRGBLinearize = m_enableSRGBLinearize,
                .alphaValue = passthroughMode == PassthroughMode::BlendLayer ? m_blendModeAlpha : m_maskModeAlpha,
                .keyColour  = m_maskModeKeyColor
            };

            const auto specializationMap = MakeSpecializationMap(fovDecodeParamPtr != nullptr, passthroughMode);
            assert(!specializationMap.empty());
            const VkSpecializationInfo speicalizationInfo{
                .mapEntryCount = (std::uint32_t)specializationMap.size(),
                .pMapEntries = specializationMap.data(),
                .dataSize = sizeof(specializationConst),
                .pData = &specializationConst
            };

            fragShaderInfo.pSpecializationInfo = &speicalizationInfo;
            m_videoStreamPipelines[pipelineIdx++].Create
            (
                m_vkDevice,
                swapChainInfo.size,
                m_videoStreamLayout,
                swapChainInfo.rp,
                videoShader,
                m_quadBuffer
            );
            // null-out pSpecializationInfo as it refers to local stack vars.
            fragShaderInfo.pSpecializationInfo = nullptr;
        }
        CreateImageDescriptorSetLayouts();
    }

    void CreateVideoStreamPipeline(const VkFormat pixFmt)
    {
        CHECK(pixFmt != VkFormat::VK_FORMAT_UNDEFINED);
        const VkSamplerYcbcrConversionCreateInfo conversionInfo {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            .pNext = nullptr,
            .format = pixFmt,
            .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
            .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
            .components {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .chromaFilter = VK_FILTER_LINEAR,
            .forceExplicitReconstruction = VK_FALSE,
        };
        CreateVideoStreamPipeline(conversionInfo);
    }

    virtual void SetEnableLinearizeRGB(const bool enable) override {
        m_enableSRGBLinearize = enable;
    }

    virtual void SetFoveatedDecode(const ALXR::FoveatedDecodeParams* fovDecParm) override {
        m_fovDecodeParams = fovDecParm ?
            std::make_shared<ALXR::FoveatedDecodeParams>(*fovDecParm) : nullptr;
    }

    virtual void SetCmdBufferWaitNextFrame(const bool enable) override {
        m_cmdBufferWaitNextFrame = enable;
    }

    virtual void SetMaskModeParams
    (
        const XrVector3f& keyColour /*= { 0.01f, 0.01f, 0.01f }*/,
        const float alpha /*= 0.3f*/
    ) override
    {
        m_maskModeKeyColor = keyColour;
        m_maskModeAlpha = alpha;
    }

    virtual void SetBlendModeParams(const float alpha /*= 0.6f*/) override
    {
        m_blendModeAlpha = alpha;
    }


    constexpr static const std::size_t VideoQueueSize = 2;

    virtual void ClearVideoTextures() override
    {
#ifdef XR_ENABLE_CUDA_INTEROP
        ClearVideoTexturesCUDA();
#endif
        m_renderTex = std::size_t(-1);
        m_currentVideoTex = 0;
        
        //m_texRendereComplete.WaitForGpu();
        m_videoTextures = { VideoTexture {}, VideoTexture {} };
#ifdef XR_USE_PLATFORM_ANDROID
        m_videoTexQueue = VideoTextureQueue(VideoQueueSize);
#else
        m_lastTexIndex = std::size_t(-1);
        textureIdx = std::size_t(-1);
#endif
        ClearImageDescriptorSetLayouts();
        for (auto& pipeline : m_videoStreamPipelines)
            pipeline.Clear();
        m_videoStreamLayout.Clear();
    }

    virtual void CreateVideoTextures(const std::size_t width, const std::size_t height, const XrPixelFormat pixfmt) override
    {
        const auto pixelFmt = MapFormat(pixfmt);
        CreateVideoStreamPipeline(pixelFmt);

        const VkDeviceSize texSize = StagingBufferSize(width, height, pixelFmt);
        for (auto& vidTex : m_videoTextures)
        {
            vidTex.width = width;
            vidTex.height = height;
            vidTex.format = pixelFmt;
            vidTex.stagingBufferSize = createStaggingBuffer
            (
                texSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                vidTex.stagingBuffer,
                vidTex.stagingBufferMemory
            );
            vidTex.texture.Create
            (
                m_vkDevice, &m_memAllocator,
                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                pixelFmt, VK_IMAGE_TILING_LINEAR, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
            );
            CHECK(vidTex.texture.IsValid());
            
            const VkSamplerYcbcrConversionInfo ycbcrConverInfo {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .pNext = nullptr,
                .conversion = m_videoStreamLayout.ycbcrSamplerConversion
            };
            const VkImageViewCreateInfo viewInfo {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &ycbcrConverInfo,
                .image = vidTex.texture.texImage,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = pixelFmt,
                .subresourceRange {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                }
            };
            CHECK_VKCMD(vkCreateImageView(m_vkDevice, &viewInfo, nullptr, &vidTex.imageView));
        }
    }

    virtual void CreateVideoTexturesMediaCodec(const std::size_t /*width*/, const std::size_t /*height*/, const XrPixelFormat /*pixfmt*/) override
    {
        // NOT USED OR REQUIRED, Please check UpdateVideoTexturesMediaCodec Instead.
#if 0
        //CreateImageDescriptorSetLayouts();
        const auto pixelFmt = MapFormat(pixfmt);
        CreateVideoStreamPipeline(pixelFmt);
        
        //const VkDeviceSize texSize = StagingBufferSize(width, height, pixelFmt);
        for (auto& vidTex : m_videoTextures)
        {
            vidTex.width = width;
            vidTex.height = height;
            vidTex.format = pixelFmt;
            vidTex.texture.Create
            (
                m_vkDevice, &m_memAllocator,
                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                pixelFmt, VK_IMAGE_TILING_OPTIMAL, 0
            );

            const VkSamplerYcbcrConversionInfo ycbcrConverInfo {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .pNext = nullptr,
                .conversion = m_videoStreamLayout.ycbcrSamplerConversion
            };
            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &ycbcrConverInfo,
                .image = vidTex.texture.texImage,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = pixelFmt,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;//VK_IMAGE_ASPECT_COLOR_BIT;
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                }
            };
            CHECK_VKCMD(vkCreateImageView(m_vkDevice, &viewInfo, nullptr, &vidTex.imageView));
        }
#endif
    }

    virtual void CreateVideoTexturesD3D11VA(const std::size_t width, const std::size_t height, const XrPixelFormat pixfmt) override
    {
#if defined(XR_USE_GRAPHICS_API_D3D11)
        CHECK_MSG((pixfmt != XrPixelFormat::G8_B8_R8_3PLANE_420 &&
            pixfmt != XrPixelFormat::G10X6_B10X6_R10X6_3PLANE_420), "3-Planes formats are not supported!");

        const auto pixelFmt = MapFormat(pixfmt);
        CreateVideoStreamPipeline(pixelFmt);

        constexpr const auto MapFormat = [](const XrPixelFormat pixfmt) -> DXGI_FORMAT {
            switch (pixfmt) {
            case XrPixelFormat::NV12: return DXGI_FORMAT_NV12;
            case XrPixelFormat::P010LE: return DXGI_FORMAT_P010;
            }
            return DXGI_FORMAT_UNKNOWN;
        };
        constexpr const auto GetSharedHandle = [](const ID3D11Texture2DPtr& newTex)
        {
            assert(newTex != nullptr);
            Microsoft::WRL::ComPtr<IDXGIResource1> DxgiResource1{};
            CHECK(SUCCEEDED(newTex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(DxgiResource1.GetAddressOf()))));
            HANDLE sharedHandle = 0;
            CHECK(SUCCEEDED(DxgiResource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle)));
            CHECK(sharedHandle != 0);
            return sharedHandle;
        };
        for (auto& vidTex : m_videoTextures)
        {
            const D3D11_TEXTURE2D_DESC descDepth {
                .Width = static_cast<UINT> (width),
                .Height = static_cast<UINT>(height),
                .MipLevels = 1,
                .ArraySize = 1,
                .Format = MapFormat(pixfmt),
                .SampleDesc {
                    .Count = 1,
                    .Quality = 0
                },
                .Usage = D3D11_USAGE_DEFAULT,
                .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                .CPUAccessFlags = 0,
                .MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX //D3D11_RESOURCE_MISC_SHARED;// D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
            };
            ID3D11Texture2DPtr newTex{};
            if (FAILED(m_d3d11vaDevice->CreateTexture2D(&descDepth, nullptr, newTex.ReleaseAndGetAddressOf())))
            {
                Log::Write(Log::Level::Info, "CreateTexture2D Failed");
                CHECK(false);
            }
            vidTex.d3d11vaSharedTexture = newTex;
            vidTex.sharedHandle = GetSharedHandle(newTex);
            CHECK(vidTex.sharedHandle != 0);

            vidTex.width = width;
            vidTex.height = height;
            vidTex.format = pixelFmt;
            vidTex.texture.CreateImportedD3D11Texture
            (
                m_vkPhysicalDevice, m_vkDevice, &m_memAllocator,
                vidTex.sharedHandle, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), pixelFmt,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_CREATE_DISJOINT_BIT
            );
            //CloseHandle(vidTex.sharedHandle);

            const VkSamplerYcbcrConversionInfo ycbcrConverInfo {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .pNext = nullptr,
                .conversion = m_videoStreamLayout.ycbcrSamplerConversion
            };
            const VkImageViewCreateInfo viewInfo {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &ycbcrConverInfo,
                .image = vidTex.texture.texImage,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = pixelFmt,
                .subresourceRange {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;//VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                }
            };
            CHECK_VKCMD(vkCreateImageView(m_vkDevice, &viewInfo, nullptr, &vidTex.imageView));
        }
#else
        (void)width; (void)height; (void)pixfmt;
#endif
    }

    bool WaitForAvailableBuffer()
    {
        //m_texRendereComplete.WaitForGpu();
        //Log::Write(Log::Level::Info, Fmt("render idx: %d, copy idx: %d", m_renderTex.load(), m_currentVideoTex.load()));
        //CHECK_HRCMD(m_texRendereComplete.Wait(m_videoTexCmdCpyQueue));
        return true;
    }

    virtual void UpdateVideoTexture(const YUVBuffer& yuvBuffer) override
    {
        const std::size_t freeIndex = m_currentVideoTex.load();
        auto& videoTex = m_videoTextures[freeIndex];

        const bool has3Planes = yuvBuffer.chroma2.data != nullptr;
        const std::size_t lumaSize    = LumaSize(videoTex.format);
        const std::size_t chromaSize  = ChromaSize(videoTex.format);
        const std::size_t chromaUSize = has3Planes ? (chromaSize / 2) : chromaSize;
        const std::size_t chromaVSize = has3Planes ? chromaUSize : 0;

        const std::size_t textureSize = videoTex.width * videoTex.height;
        const VkDeviceSize uPlaneOffset = textureSize * lumaSize;
        const VkDeviceSize vPlaneOffset = has3Planes ? uPlaneOffset + ((textureSize / 2) * chromaUSize) : 0;

        void* data = nullptr;
        vkMapMemory(m_vkDevice, videoTex.stagingBufferMemory, 0, videoTex.stagingBufferSize, 0, &data);
        {
            constexpr const auto copy2d = []
            (
                std::uint8_t* dst, const std::uint8_t* src,
                const std::size_t pitchInBytes, const std::size_t width, const std::size_t height, const std::size_t formatSize
            )
            {
                const std::size_t lineDstSize = width * formatSize;
                if (lineDstSize == pitchInBytes) {
                    //using namespace std::execution;
                    //std::uninitialized_copy_n(par_unseq, src, lineDstSize * height, dst);
                    std::memcpy(dst, src, lineDstSize * height);
                    return;
                }
                for (std::size_t h = 0; h < height; ++h)
                {
                    auto lineDstPtr = dst + (lineDstSize * h);
                    const auto lineSrcPtr = src + (pitchInBytes * h);
                    std::memcpy(lineDstPtr, lineSrcPtr, lineDstSize);
                    //std::copy_n(par_unseq, src, chromaLineSize, dst);
                }
            };

            const auto& luma = yuvBuffer.luma;
            const auto yPlanePtr = reinterpret_cast<std::uint8_t*>(data);               
            copy2d
            (
                yPlanePtr, reinterpret_cast<const std::uint8_t*>(luma.data),
                luma.pitch, videoTex.width, luma.height, lumaSize
            );

            const auto& chroma = yuvBuffer.chroma;
            const auto uPlanePtr = yPlanePtr + uPlaneOffset;
            copy2d
            (
                uPlanePtr, reinterpret_cast<const std::uint8_t*>(chroma.data),
                chroma.pitch, videoTex.width / 2, chroma.height, chromaUSize
            );

            if (has3Planes)
            {
                const auto& chromaV = yuvBuffer.chroma2;
                const auto vPlanePtr = yPlanePtr + vPlaneOffset;
                copy2d
                (
                    vPlanePtr, reinterpret_cast<const std::uint8_t*>(chromaV.data),
                    chromaV.pitch, videoTex.width / 2, chromaV.height, chromaVSize
                );
            }
        }
        vkUnmapMemory(m_vkDevice, videoTex.stagingBufferMemory);

        m_videoCpyCmdBuffer.Reset();
        m_videoCpyCmdBuffer.Begin();

        videoTex.texture.TransitionLayout(m_videoCpyCmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        {
            const VkBufferImageCopy buffImgCopy{
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource {
                    .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = {
                    static_cast<std::uint32_t>(videoTex.width),
                    static_cast<std::uint32_t>(videoTex.height),
                    1
                }
            };
            std::array<VkBufferImageCopy, 3> region{ buffImgCopy, buffImgCopy, buffImgCopy };
            region[1].bufferOffset = uPlaneOffset;
            region[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            region[1].imageExtent = {
                static_cast<std::uint32_t>(videoTex.width / 2),
                static_cast<std::uint32_t>(videoTex.height / 2),
                1
            };
            region[2] = region[1];
            region[2].bufferOffset = vPlaneOffset;
            region[2].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
            const auto regionCount = static_cast<std::uint32_t>(has3Planes ? region.size() : 2);
            const auto texImage = videoTex.texture.texImage;
            vkCmdCopyBufferToImage(m_videoCpyCmdBuffer.buf, videoTex.stagingBuffer, texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, region.data());
        }
        videoTex.texture.TransitionLayout(m_videoCpyCmdBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        m_videoCpyCmdBuffer.End();
        m_videoCpyCmdBuffer.Exec(m_VideoCpyQueue);//ExecSignalers<1>(m_VideoCpyQueue, { &m_texCopy }); //Exec(m_VideoCpyQueue);
        m_videoCpyCmdBuffer.Wait();
        
        videoTex.frameIndex = yuvBuffer.frameIndex;
        m_currentVideoTex.store((freeIndex + 1) % VideoTexCount);
        m_renderTex.store(freeIndex);
    }

    virtual void BeginVideoView() override
    {
#ifdef XR_USE_PLATFORM_ANDROID
        VideoTexture newVideoTex{};

        if (m_noFrameSkip) {
            m_videoTexQueue.try_dequeue(newVideoTex);
        } else {
            std::size_t popCount = 0;
            while (m_videoTexQueue.try_dequeue(newVideoTex) && popCount < VideoQueueSize) {
                ++popCount;
            }
        }

        if (!m_noServerFramerateLock && !newVideoTex.IsValid()) {
            using namespace std::literals::chrono_literals;
            constexpr static const auto QueueTextureWaitTime = 100ms;
            m_videoTexQueue.wait_dequeue_timed(newVideoTex, QueueTextureWaitTime);
        }

        if (newVideoTex.IsValid()) {
            auto& newCurrentTexture = m_videoTextures[VidTextureIndex::Current];
            m_videoTextures[VidTextureIndex::DeferredDelete] = std::move(newCurrentTexture);
            newCurrentTexture = std::move(newVideoTex);
            UpdateVideoTextureBinding(newCurrentTexture);
        }
#else
        textureIdx = m_renderTex.load();
        if (textureIdx == std::size_t(-1) || textureIdx == m_lastTexIndex)
            return;
        UpdateVideoTextureBinding(textureIdx);
        m_lastTexIndex = textureIdx;
#endif
    }

    virtual void EndVideoView() override
    {
#ifdef XR_USE_PLATFORM_ANDROID
#else
#endif
    }
    
    virtual std::uint64_t GetVideoFrameIndex() const override {
#ifdef XR_USE_PLATFORM_ANDROID
        return m_videoTextures[VidTextureIndex::Current].frameIndex;
#else
        return textureIdx == std::uint64_t(-1) ?
            textureIdx :
            m_videoTextures[textureIdx].frameIndex;
#endif
    }

#ifdef XR_USE_PLATFORM_ANDROID

    static inline void LogSuggestedYCBCRConversionParams(const Texture::AHBufferFormatProperties& formatProperties) {
        constexpr const auto YcbcrModelConversionToStr = [](const VkSamplerYcbcrModelConversion ymc) {
            switch (ymc) {
            case VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY_KHR:
            //case VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY:
                return "RGB_IDENTITY";
            case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY_KHR:
            //case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY:
                return "YCBCR_IDENTITY";
            case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709_KHR:
            //case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709:
                return "YCBCR_709";
            case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601_KHR:
            //case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:
                return "YCBCR_601";
            case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020_KHR:
            //case VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020:
                return "YCBCR_2020";
            default: return "unknown";
            }            
        };
        constexpr const auto YcbcrRangeToStr = [](const VkSamplerYcbcrRange range) {
            switch (range) {
            //case VK_SAMPLER_YCBCR_RANGE_ITU_FULL:
            case VK_SAMPLER_YCBCR_RANGE_ITU_FULL_KHR:
                return "ITU_FULL";
            //case VK_SAMPLER_YCBCR_RANGE_ITU_NARROW:
            case VK_SAMPLER_YCBCR_RANGE_ITU_NARROW_KHR:
                return "ITU_NARROW";
            default: return "unknown";
            }

        };
        constexpr const auto ChromaLocationToStr = [](const VkChromaLocation chromaLoc) {
            switch (chromaLoc) {
            //case VK_CHROMA_LOCATION_COSITED_EVEN:
            case VK_CHROMA_LOCATION_COSITED_EVEN_KHR:
                return "COSITED_EVEN";
            //case VK_CHROMA_LOCATION_MIDPOINT:
            case VK_CHROMA_LOCATION_MIDPOINT_KHR:
                return "MIDPOINT";
            default: return "unknown";
            }
        };
        Log::Write(Log::Level::Info, Fmt("Suggested Ycbcr conversion parameters:\n"
            "\tYcbcrModel: %s\n"
            "\tYcbcrRange: %s\n"
            "\tX-ChromaOffset: %s\n"
            "\tY-ChromaOffset: %s",
            YcbcrModelConversionToStr(formatProperties.suggestedYcbcrModel),
            YcbcrRangeToStr(formatProperties.suggestedYcbcrRange),
            ChromaLocationToStr(formatProperties.suggestedXChromaOffset),
            ChromaLocationToStr(formatProperties.suggestedYChromaOffset)));
    }

    virtual void UpdateVideoTextureMediaCodec(const YUVBuffer& yuvBuffer) override
    {
        AImage* img = reinterpret_cast<AImage*>(yuvBuffer.luma.data);
        if (img == nullptr)
            return;
        if (yuvBuffer.frameIndex == std::uint64_t(-1)) {
            AImage_delete(img);
            return;
        }

        AHardwareBuffer* hwBuff = nullptr;
        if (AImage_getHardwareBuffer(img, &hwBuff) != AMEDIA_OK || hwBuff == nullptr) {
            AImage_delete(img);
            return;
        }

        AHardwareBuffer_Desc hwbuffDesc{};
        AHardwareBuffer_describe(hwBuff, &hwbuffDesc);

        //auto start = GetSteadyTimestampUs();
        VideoTexture newVideoTex{};
        newVideoTex.ndkImage = img;
        newVideoTex.frameIndex = yuvBuffer.frameIndex;
        newVideoTex.width = hwbuffDesc.width;   // yuvBuffer.luma.pitch;
        newVideoTex.height = hwbuffDesc.height; // yuvBuffer.luma.height;
        
        Texture::AHBufferFormatProperties formatProperties;
        newVideoTex.texture.CreateAHardwareBufferImported
        (
            m_vkDevice, m_vkInstance, &m_memAllocator,
            hwBuff, newVideoTex.width, newVideoTex.height,
            formatProperties
        );

        if (m_videoStreamLayout.IsNull()) {
            const VkExternalFormatANDROID externalFormat{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
                .pNext = nullptr,
                .externalFormat = formatProperties.externalFormat
            };
            const VkSamplerYcbcrConversionCreateInfo samplerInfo{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                .pNext = &externalFormat,
                .format = formatProperties.format,
                .ycbcrModel = formatProperties.suggestedYcbcrModel,
                .ycbcrRange = formatProperties.suggestedYcbcrRange,
                .components = formatProperties.samplerYcbcrConversionComponents,
                .xChromaOffset = formatProperties.suggestedXChromaOffset,
                .yChromaOffset = formatProperties.suggestedYChromaOffset,
                .chromaFilter = ((formatProperties.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0) ?
                    VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                .forceExplicitReconstruction = (formatProperties.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_BIT) != 0
            };
            LogSuggestedYCBCRConversionParams(formatProperties);
            CreateVideoStreamPipeline(samplerInfo);
        }
        CHECK(!m_videoStreamLayout.IsNull() && m_videoStreamLayout.ycbcrSamplerConversion != VK_NULL_HANDLE);

        const VkSamplerYcbcrConversionInfo ycbcrConverInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            .pNext = nullptr,
            .conversion = m_videoStreamLayout.ycbcrSamplerConversion,
        };
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &ycbcrConverInfo,
            .image = newVideoTex.texture.texImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formatProperties.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        CHECK_VKCMD(vkCreateImageView(m_vkDevice, &viewInfo, nullptr, &newVideoTex.imageView));

        using namespace std::literals::chrono_literals;
        constexpr static const auto QueueTextureWaitTime = 100ms;
        if (!m_videoTexQueue.wait_enqueue_timed(std::move(newVideoTex), QueueTextureWaitTime)) {
            Log::Write(Log::Level::Warning, Fmt("Waiting to queue decoded video frame (pts: %llu) timed-out after %lld seconds, this frame will be ignored", yuvBuffer.frameIndex, QueueTextureWaitTime.count()));
        }
    }
#endif

    virtual void UpdateVideoTextureD3D11VA(const YUVBuffer& yuvBuffer) override
    {
#if defined(XR_USE_GRAPHICS_API_D3D11)
        const std::size_t freeIndex = m_currentVideoTex.load();
        {
            /*const*/ auto& videoTex = m_videoTextures[freeIndex];
            videoTex.frameIndex = yuvBuffer.frameIndex;
            auto dstVideoTexture = videoTex.d3d11vaSharedTexture;
            CHECK(dstVideoTexture != nullptr);

            D3D11_TEXTURE2D_DESC desc;
            dstVideoTexture->GetDesc(&desc);

            const ID3D11Texture2DPtr src_texture = reinterpret_cast<ID3D11Texture2D*>(yuvBuffer.luma.data);
            const auto texture_index = (UINT)reinterpret_cast<std::intptr_t>(yuvBuffer.chroma.data);

            ID3D11DeviceContext* devCtx = nullptr;
            m_d3d11vaDevice->GetImmediateContext(&devCtx);
            CHECK(devCtx);

            const D3D11_BOX sourceRegion {
                .left = 0,
                .top = 0,
                .front = 0,
                .right = desc.Width,                
                .bottom = desc.Height,                
                .back = 1,
            };
            m_videoCpyCmdBuffer.Wait();
            m_videoCpyCmdBuffer.Reset();
            m_videoCpyCmdBuffer.Begin();

            videoTex.texture.TransitionLayout(m_videoCpyCmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            devCtx->CopySubresourceRegion(dstVideoTexture.Get(), 0, 0, 0, 0, src_texture.Get(), texture_index, &sourceRegion);
            // Flush to submit the 11 command list to the shared command queue.
            devCtx->Flush();

            videoTex.texture.TransitionLayout(m_videoCpyCmdBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            m_videoCpyCmdBuffer.End();
            m_videoCpyCmdBuffer.ExecSignalers<1>(m_VideoCpyQueue, { &m_texCopy });//Exec(m_VideoCpyQueue);//ExecSignalers<1>(m_VideoCpyQueue, { &m_texCopy }); //Exec(m_VideoCpyQueue);
        }

        m_currentVideoTex.store((freeIndex + 1) % m_videoTextures.size());
        m_renderTex.store(freeIndex);
#else
        (void)yuvBuffer;
#endif
    }

    virtual void RenderVideoMultiView
    (
        const std::array<XrCompositionLayerProjectionView, 2>& /*layerViews*/,
        const XrSwapchainImageBaseHeader* swapchainImage, const std::int64_t /*swapchainFormat*/,
        const PassthroughMode newMode /*= PassthroughMode::None*/
    ) override
    {
        assert(m_isMultiViewSupported);
        RenderViewImpl(swapchainImage, [&, this](const std::uint32_t imageIndex, auto& swapchainContext)
        {
            const auto& clearValues = VideoClearValues[ClearValueIndex(newMode)];
            VkRenderPassBeginInfo renderPassBeginInfo{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .pNext = nullptr,
                .clearValueCount = (uint32_t)clearValues.size(),
                .pClearValues = clearValues.data()
            };
            // Bind and clear eye render target
            swapchainContext.BindRenderTarget(imageIndex, /*out*/ renderPassBeginInfo);

#ifdef XR_USE_PLATFORM_ANDROID
            auto& currentTexture = m_videoTextures[VidTextureIndex::Current];
            if (currentTexture.texture.texImage == VK_NULL_HANDLE)
                return;
            currentTexture.texture.TransitionLayout(m_cmdBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
#else
            if (textureIdx == std::size_t(-1))
                return;
#endif
            vkCmdBeginRenderPass(m_cmdBuffer.buf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_videoStreamPipelines[static_cast<std::size_t>(newMode)].pipe);
            vkCmdBindDescriptorSets(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_videoStreamLayout.layout, 0, 1, m_descriptorSets.data(), 0, nullptr);

            // Bind index and vertex buffers
            vkCmdBindIndexBuffer(m_cmdBuffer.buf, m_quadBuffer.idxBuf, 0, VK_INDEX_TYPE_UINT16);
            constexpr static const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(m_cmdBuffer.buf, 0, 1, &m_quadBuffer.vtxBuf, &offset);

            vkCmdDrawIndexed(m_cmdBuffer.buf, m_quadBuffer.count.idx, 1, 0, 0, 0);
            vkCmdEndRenderPass(m_cmdBuffer.buf);
        });
    }

    virtual void RenderVideoView
    (
        const std::uint32_t viewID, const XrCompositionLayerProjectionView& /*layerView*/,
        const XrSwapchainImageBaseHeader* swapchainImage, const std::int64_t /*swapchainFormat*/,
        const PassthroughMode mode /*= PassthroughMode::None*/
    ) override
    {
        RenderViewImpl(swapchainImage, [&, this](const std::uint32_t imageIndex, auto& swapchainContext)
        {
            const auto& clearValues = VideoClearValues[ClearValueIndex(mode)];
            VkRenderPassBeginInfo renderPassBeginInfo{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .pNext = nullptr,
                .clearValueCount = (uint32_t)clearValues.size(),
                .pClearValues = clearValues.data()
            };
            // Bind and clear eye render target
            swapchainContext.BindRenderTarget(imageIndex, /*out*/ renderPassBeginInfo);

#ifdef XR_USE_PLATFORM_ANDROID
            auto& currentTexture = m_videoTextures[VidTextureIndex::Current];
            if (currentTexture.texture.texImage == VK_NULL_HANDLE)
                return;
            currentTexture.texture.TransitionLayout(m_cmdBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
#else
            if (textureIdx == std::size_t(-1))
                return;
#endif
            vkCmdBeginRenderPass(m_cmdBuffer.buf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_videoStreamPipelines[static_cast<std::size_t>(mode)].pipe);
            vkCmdBindDescriptorSets(m_cmdBuffer.buf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_videoStreamLayout.layout, 0, 1, m_descriptorSets.data(), 0, nullptr);

            // Bind index and vertex buffers
            vkCmdBindIndexBuffer(m_cmdBuffer.buf, m_quadBuffer.idxBuf, 0, VK_INDEX_TYPE_UINT16);
            constexpr static const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(m_cmdBuffer.buf, 0, 1, &m_quadBuffer.vtxBuf, &offset);

            const ViewProjectionUniform mvp1{ .ViewID = viewID };
            vkCmdPushConstants(m_cmdBuffer.buf, m_videoStreamLayout.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ViewProjectionUniform), &mvp1);

            vkCmdDrawIndexed(m_cmdBuffer.buf, m_quadBuffer.count.idx, 1, 0, 0, 0);
            vkCmdEndRenderPass(m_cmdBuffer.buf);
        });
    }

    virtual inline void SetEnvironmentBlendMode(const XrEnvironmentBlendMode newMode) override {
        m_clearColorIndex = (newMode - 1);
    }

    virtual inline bool IsMultiViewEnabled() const override {
        return m_isMultiViewSupported;
    }
    
    virtual ~VulkanGraphicsPlugin() override {
        ClearImageDescriptorSetLayouts();
        Log::Write(Log::Level::Verbose, "VulkanGraphicsPlugin destroyed.");
    }

#include "cuda/vulkancuda_interop.inl"

   protected:
    XrGraphicsBindingVulkan2KHR m_graphicsBinding{
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .next = nullptr,
        .instance= VK_NULL_HANDLE,
        .physicalDevice = VK_NULL_HANDLE,
        .device = VK_NULL_HANDLE,
        .queueFamilyIndex = 0,
        .queueIndex = 0,
    };
    std::list<SwapchainImageContext> m_swapchainImageContexts;
    std::map<const XrSwapchainImageBaseHeader*, SwapchainImageContext*> m_swapchainImageContextMap;

    VkInstance m_vkInstance{VK_NULL_HANDLE};
    VkPhysicalDevice m_vkPhysicalDevice{VK_NULL_HANDLE};
    VkDevice m_vkDevice{VK_NULL_HANDLE};
    uint32_t m_queueFamilyIndex = 0;
    VkQueue m_vkQueue{VK_NULL_HANDLE};
    
    MemoryAllocator m_memAllocator{};
    ShaderProgram m_shaderProgram{};
    CmdBuffer m_cmdBuffer{};
    PipelineLayout m_pipelineLayout{};
    VertexBuffer<Geometry::Vertex> m_drawBuffer{};
    bool m_isMultiViewSupported = false;

// BEGIN VIDEO STREAM DATA /////////////////////////////////////////////////////////////
    std::array<std::uint8_t, VK_UUID_SIZE> m_vkDeviceUUID{};
    std::array<std::uint8_t, VK_UUID_SIZE> m_vkDeviceLUID{};

#if defined(XR_USE_GRAPHICS_API_D3D11)
    Microsoft::WRL::ComPtr<ID3D11Device>        m_d3d11vaDevice{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11vaDeviceCtx{};
#endif

    uint32_t m_queueFamilyIndexVideoCpy = 0;
    VkQueue m_VideoCpyQueue{ VK_NULL_HANDLE };

    VkDescriptorPool m_descriptorPool{ VK_NULL_HANDLE };
    std::vector<VkDescriptorSet> m_descriptorSets{};

    CmdBuffer m_videoCpyCmdBuffer{};
    
    using VideoShaderList = std::array<ShaderProgram, size_t(PassthroughMode::TypeCount)>;
    using VideoShaderMap  = std::array<VideoShaderList, size_t(VideoFragShaderType::TypeCount)>;
    VideoShaderMap m_videoShaders {};
    
    VertexBuffer<Geometry::QuadVertex> m_quadBuffer{};
    PipelineLayout m_videoStreamLayout{};
    using PipelineList = std::array<Pipeline, size_t(PassthroughMode::TypeCount)>;
    PipelineList m_videoStreamPipelines{};
    bool m_enableSRGBLinearize = true;

    using FoveatedDecodeParamsPtr = std::shared_ptr<ALXR::FoveatedDecodeParams>;
    FoveatedDecodeParamsPtr m_fovDecodeParams{};

    XrVector3f m_maskModeKeyColor = { 0.01f, 0.01f, 0.01f };
    float      m_maskModeAlpha = 0.3f;
    float      m_blendModeAlpha = 0.6f;

    bool m_cmdBufferWaitNextFrame = true;

    constexpr static const std::size_t VideoTexCount = 2;

#ifndef XR_USE_PLATFORM_ANDROID
    SemaphoreTimeline m_texRendereComplete{};
    SemaphoreTimeline m_texCopy{};
#endif

#if defined(XR_USE_GRAPHICS_API_D3D11)
    using ID3D11Texture2DPtr = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
#endif

    struct VideoTexture {

        Texture texture{};

        VkBuffer stagingBuffer{ VK_NULL_HANDLE };
        VkDeviceMemory stagingBufferMemory{ VK_NULL_HANDLE };
        VkDeviceSize stagingBufferSize {0};

        VkImageView imageView{ VK_NULL_HANDLE };

#if defined(XR_USE_GRAPHICS_API_D3D11)
        ID3D11Texture2DPtr d3d11vaSharedTexture{};
        HANDLE        sharedHandle{};
#endif

#ifdef XR_USE_PLATFORM_ANDROID
        AImage* ndkImage = nullptr;
#endif
        std::uint64_t frameIndex = std::uint64_t(-1);
        std::size_t width = 0;
        std::size_t height = 0;
        VkFormat    format = VK_FORMAT_UNDEFINED;

        inline VideoTexture() noexcept = default;
        inline ~VideoTexture() noexcept {
            Clear();
        }

        inline VideoTexture(VideoTexture&& other) noexcept
        : VideoTexture()
        {
            texture = std::move(other.texture);
            std::swap(stagingBuffer, other.stagingBuffer);
            std::swap(stagingBufferMemory, other.stagingBufferMemory);
            std::swap(stagingBufferSize, other.stagingBufferSize);
            std::swap(imageView, other.imageView);
            std::swap(frameIndex, other.frameIndex);
            std::swap(width, other.width);
            std::swap(height, other.height);
            std::swap(format, other.format);
#if defined(XR_USE_GRAPHICS_API_D3D11)
            d3d11vaSharedTexture.Swap(other.d3d11vaSharedTexture);
            std::swap(sharedHandle, other.sharedHandle);
#endif
#ifdef XR_USE_PLATFORM_ANDROID
            std::swap(ndkImage, other.ndkImage);
#endif
        }

        inline VideoTexture& operator=(VideoTexture&& other) noexcept
        {
            if (this == &other)
                return *this;
            Clear();
            texture = std::move(other.texture);
            std::swap(stagingBuffer, other.stagingBuffer);
            std::swap(stagingBufferMemory, other.stagingBufferMemory);
            std::swap(stagingBufferSize, other.stagingBufferSize);
            std::swap(imageView, other.imageView);
            std::swap(frameIndex, other.frameIndex);
            std::swap(width, other.width);
            std::swap(height, other.height);
            std::swap(format, other.format);
#if defined(XR_USE_GRAPHICS_API_D3D11)
            d3d11vaSharedTexture.Swap(other.d3d11vaSharedTexture);
            std::swap(sharedHandle, other.sharedHandle);
#endif
#ifdef XR_USE_PLATFORM_ANDROID
            std::swap(ndkImage, other.ndkImage);
#endif
            return *this;
        }

        inline VideoTexture(const VideoTexture&) noexcept = delete;
        inline VideoTexture& operator=(const VideoTexture&) noexcept = delete;

        void Clear()
        {
#if defined(XR_USE_GRAPHICS_API_D3D11)
            d3d11vaSharedTexture.Reset();
            sharedHandle = 0;
#endif
            const auto vkDevice = texture.m_vkDevice;
            if (vkDevice != VK_NULL_HANDLE)
            {
                if (stagingBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(vkDevice, stagingBuffer, nullptr);
                }
                if (stagingBufferMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(vkDevice, stagingBufferMemory, nullptr);
                }
                if (imageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(vkDevice, imageView, nullptr);
                }
            }
            stagingBuffer = VK_NULL_HANDLE;
            stagingBufferMemory = VK_NULL_HANDLE;
            stagingBufferSize = 0;
            imageView = VK_NULL_HANDLE;
            texture.Clear();
            frameIndex = std::uint64_t(-1);
            width = 0;
            height = 0;
            format = VK_FORMAT_UNDEFINED;
#ifdef XR_USE_PLATFORM_ANDROID
            if (ndkImage != nullptr) {
                //Log::Write(Log::Level::Info, "Deleteing AImage!!!");
                AImage_delete(ndkImage);
            }
            ndkImage = nullptr;
#endif
        }

        inline bool IsValid() const {
            return texture.IsValid();
        }

        constexpr inline std::size_t StagingBufferSize() const
        {
            return VulkanGraphicsPlugin::StagingBufferSize(width, height, format);
        }
    };

    void UpdateVideoTextureBinding(const VideoTexture& vidTexture)
    {
        CHECK(m_videoStreamLayout.textureSampler != VK_NULL_HANDLE);
        CHECK(vidTexture.imageView != VK_NULL_HANDLE);
        const auto swapChainCount = static_cast<std::uint32_t>(m_swapchainImageContexts.back().swapchainImages.size());
        for (size_t i = 0; i < m_descriptorSets.size(); ++i)
        {
            const VkDescriptorImageInfo imageInfo{
                .sampler = m_videoStreamLayout.textureSampler,
                .imageView = vidTexture.imageView,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            };
            const std::array<VkWriteDescriptorSet, 1> descriptorWrites{
                VkWriteDescriptorSet {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = m_descriptorSets[i],
                    .dstBinding = 0,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,                
                    .pImageInfo = &imageInfo,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr
                }
            };
            vkUpdateDescriptorSets(m_vkDevice, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        }
    }

    inline void UpdateVideoTextureBinding(const std::size_t vidTexIndex)
    {
        UpdateVideoTextureBinding(m_videoTextures[vidTexIndex]);
    }

    static_assert(VideoTexCount >= 2);
    std::array<VideoTexture, VideoTexCount>  m_videoTextures{};
    std::atomic<std::size_t>            m_currentVideoTex{ 0 },
                                        m_renderTex{ std::size_t(-1) };

#ifndef XR_USE_PLATFORM_ANDROID
    std::size_t m_lastTexIndex = std::size_t(-1);
    std::size_t textureIdx = std::size_t(-1);
#else
    enum VidTextureIndex : std::size_t {
        Current,
        DeferredDelete
    };
    using VideoTextureQueue = moodycamel::BlockingReaderWriterCircularBuffer<VideoTexture>; //atomic_queue::AtomicQueue2<VideoTexture, 2>;// moodycamel::BlockingReaderWriterCircularBuffer<VideoTexture>; // xrconcurrency::concurrent_queue<VideoTexture>; //
    VideoTextureQueue m_videoTexQueue{ VideoQueueSize };
#endif

    static_assert(XR_ENVIRONMENT_BLEND_MODE_OPAQUE == 1);
    std::size_t m_clearColorIndex{ (XR_ENVIRONMENT_BLEND_MODE_OPAQUE - 1) };
    
    bool m_noServerFramerateLock = false;
    bool m_noFrameSkip = false;

// END VIDEO STREAM DATA /////////////////////////////////////////////////////////////

#if defined(USE_MIRROR_WINDOW)
    Swapchain m_swapchain{};
#endif

    PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT{nullptr};
    PFN_vkDestroyDebugReportCallbackEXT vkDestroyDebugReportCallbackEXT{nullptr};
    VkDebugReportCallbackEXT m_vkDebugReporter{VK_NULL_HANDLE};

    VkBool32 debugReport(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t /*location*/,
                         int32_t /*messageCode*/, const char* pLayerPrefix, const char* pMessage) {
        std::string flagNames;
        std::string objName;
        Log::Level level = Log::Level::Error;

        if ((flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) != 0u) {
            flagNames += "DEBUG:";
            level = Log::Level::Verbose;
        }
        if ((flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) != 0u) {
            flagNames += "INFO:";
            level = Log::Level::Info;
        }
        if ((flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) != 0u) {
            flagNames += "PERF:";
            level = Log::Level::Warning;
        }
        if ((flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) != 0u) {
            flagNames += "WARN:";
            level = Log::Level::Warning;
        }
        if ((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0u) {
            flagNames += "ERROR:";
            level = Log::Level::Error;
        }

#define LIST_OBJECT_TYPES(_) \
    _(UNKNOWN)               \
    _(INSTANCE)              \
    _(PHYSICAL_DEVICE)       \
    _(DEVICE)                \
    _(QUEUE)                 \
    _(SEMAPHORE)             \
    _(COMMAND_BUFFER)        \
    _(FENCE)                 \
    _(DEVICE_MEMORY)         \
    _(BUFFER)                \
    _(IMAGE)                 \
    _(EVENT)                 \
    _(QUERY_POOL)            \
    _(BUFFER_VIEW)           \
    _(IMAGE_VIEW)            \
    _(SHADER_MODULE)         \
    _(PIPELINE_CACHE)        \
    _(PIPELINE_LAYOUT)       \
    _(RENDER_PASS)           \
    _(PIPELINE)              \
    _(DESCRIPTOR_SET_LAYOUT) \
    _(SAMPLER)               \
    _(DESCRIPTOR_POOL)       \
    _(DESCRIPTOR_SET)        \
    _(FRAMEBUFFER)           \
    _(COMMAND_POOL)          \
    _(SURFACE_KHR)           \
    _(SWAPCHAIN_KHR)         \
    _(DISPLAY_KHR)           \
    _(DISPLAY_MODE_KHR)

        switch (objectType) {
            default:
#define MK_OBJECT_TYPE_CASE(name)                  \
    case VK_DEBUG_REPORT_OBJECT_TYPE_##name##_EXT: \
        objName = #name;                           \
        break;
                LIST_OBJECT_TYPES(MK_OBJECT_TYPE_CASE)

#if VK_HEADER_VERSION >= 46
                MK_OBJECT_TYPE_CASE(DESCRIPTOR_UPDATE_TEMPLATE_KHR)
#endif
#if VK_HEADER_VERSION >= 70
                MK_OBJECT_TYPE_CASE(DEBUG_REPORT_CALLBACK_EXT)
#endif
        }

        if ((objectType == VK_DEBUG_REPORT_OBJECT_TYPE_INSTANCE_EXT) && (strcmp(pLayerPrefix, "Loader Message") == 0) &&
            (strncmp(pMessage, "Device Extension:", 17) == 0)) {
            return VK_FALSE;
        }

        Log::Write(level, Fmt("%s (%s 0x%llx) [%s] %s", flagNames.c_str(), objName.c_str(), object, pLayerPrefix, pMessage));
        if ((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0u) {
            return VK_FALSE;
        }
        if ((flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) != 0u) {
            return VK_FALSE;
        }
        return VK_FALSE;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugReportThunk(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
                                                           uint64_t object, size_t location, int32_t messageCode,
                                                           const char* pLayerPrefix, const char* pMessage, void* pUserData) {
        return static_cast<VulkanGraphicsPlugin*>(pUserData)->debugReport(flags, objectType, object, location, messageCode,
                                                                          pLayerPrefix, pMessage);
    }

    virtual XrStructureType GetGraphicsBindingType() const { return XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR; }
    virtual XrStructureType GetSwapchainImageType() const { return XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR; }

    virtual XrResult CreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo,
                                             VkInstance* vulkanInstance, VkResult* vulkanResult) {
        PFN_xrCreateVulkanInstanceKHR pfnCreateVulkanInstanceKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVulkanInstanceKHR)));

        return pfnCreateVulkanInstanceKHR(instance, createInfo, vulkanInstance, vulkanResult);
    }

    virtual XrResult CreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo,
                                           VkDevice* vulkanDevice, VkResult* vulkanResult) {
        PFN_xrCreateVulkanDeviceKHR pfnCreateVulkanDeviceKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVulkanDeviceKHR)));

        return pfnCreateVulkanDeviceKHR(instance, createInfo, vulkanDevice, vulkanResult);
    }

    virtual XrResult GetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
                                                 VkPhysicalDevice* vulkanPhysicalDevice) {
        PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVulkanGraphicsDevice2KHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsDevice2KHR)));

        return pfnGetVulkanGraphicsDevice2KHR(instance, getInfo, vulkanPhysicalDevice);
    }

    virtual XrResult GetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId,
                                                       XrGraphicsRequirementsVulkan2KHR* graphicsRequirements) {
        PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVulkanGraphicsRequirements2KHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsRequirements2KHR)));

        return pfnGetVulkanGraphicsRequirements2KHR(instance, systemId, graphicsRequirements);
    }
};

// A compatibility class that implements the KHR_vulkan_enable2 functionality on top of KHR_vulkan_enable
struct VulkanGraphicsPluginLegacy final : public VulkanGraphicsPlugin {
    VulkanGraphicsPluginLegacy(const std::shared_ptr<Options>& options, std::shared_ptr<IPlatformPlugin> platformPlugin)
        : VulkanGraphicsPlugin(options, platformPlugin) {
        m_graphicsBinding.type = GetGraphicsBindingType();
    };

    std::vector<std::string> GetInstanceExtensions() const override { return {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME}; }
    virtual XrStructureType GetGraphicsBindingType() const override { return XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR; }
    virtual XrStructureType GetSwapchainImageType() const override { return XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR; }

    virtual XrResult CreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo,
                                             VkInstance* vulkanInstance, VkResult* vulkanResult) override {
        PFN_xrGetVulkanInstanceExtensionsKHR pfnGetVulkanInstanceExtensionsKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanInstanceExtensionsKHR)));

        uint32_t extensionNamesSize = 0;
        CHECK_XRCMD(pfnGetVulkanInstanceExtensionsKHR(instance, createInfo->systemId, 0, &extensionNamesSize, nullptr));

        std::vector<char> extensionNames(extensionNamesSize);
        if (extensionNamesSize > 0) {
            CHECK_XRCMD(pfnGetVulkanInstanceExtensionsKHR(instance, createInfo->systemId, extensionNamesSize, &extensionNamesSize,
                &extensionNames[0]));
        }

        {
            // Note: This cannot outlive the extensionNames above, since it's just a collection of views into that string!
            std::vector<const char*> extensions{};
            if (extensionNamesSize > 0) {
                extensions = ParseExtensionString(&extensionNames[0]);
            }

            // Merge the runtime's request with the applications requests
            for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; ++i) {
                extensions.push_back(createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
            }

            VkInstanceCreateInfo instInfo{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pNext=nullptr};
            memcpy(&instInfo, createInfo->vulkanCreateInfo, sizeof(instInfo));
            instInfo.enabledExtensionCount = (uint32_t)extensions.size();
            instInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

            auto pfnCreateInstance = (PFN_vkCreateInstance)createInfo->pfnGetInstanceProcAddr(nullptr, "vkCreateInstance");
            *vulkanResult = pfnCreateInstance(&instInfo, createInfo->vulkanAllocator, vulkanInstance);
        }

        return XR_SUCCESS;
    }

    virtual XrResult CreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo,
                                           VkDevice* vulkanDevice, VkResult* vulkanResult) override {
        PFN_xrGetVulkanDeviceExtensionsKHR pfnGetVulkanDeviceExtensionsKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanDeviceExtensionsKHR)));

        uint32_t deviceExtensionNamesSize = 0;
        CHECK_XRCMD(pfnGetVulkanDeviceExtensionsKHR(instance, createInfo->systemId, 0, &deviceExtensionNamesSize, nullptr));
        std::vector<char> deviceExtensionNames(deviceExtensionNamesSize);
        if (deviceExtensionNamesSize > 0) {
            CHECK_XRCMD(pfnGetVulkanDeviceExtensionsKHR(instance, createInfo->systemId, deviceExtensionNamesSize,
                &deviceExtensionNamesSize, &deviceExtensionNames[0]));
        }
        {
            // Note: This cannot outlive the extensionNames above, since it's just a collection of views into that string!
            std::vector<const char*> extensions{};
            if (deviceExtensionNamesSize > 0) {
                extensions = ParseExtensionString(&deviceExtensionNames[0]);
            }

            // Merge the runtime's request with the applications requests
            for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; ++i) {
                extensions.push_back(createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
            }

            // NOTE: Calling function's passed XrVulkanDeviceCreateInfoKHR::VkDeviceCreateInfo will be shallo cloned
            //       pointer chains will still refer to parent stack, this is intentional to maintain any extension structures/flags.

            VkDeviceCreateInfo deviceInfo{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext=nullptr};
            std::memcpy(&deviceInfo, createInfo->vulkanCreateInfo, sizeof(deviceInfo));
            deviceInfo.enabledExtensionCount = (uint32_t)extensions.size();
            deviceInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

            auto pfnCreateDevice = (PFN_vkCreateDevice)createInfo->pfnGetInstanceProcAddr(m_vkInstance, "vkCreateDevice");
            *vulkanResult = pfnCreateDevice(m_vkPhysicalDevice, &deviceInfo, createInfo->vulkanAllocator, vulkanDevice);
        }

        return XR_SUCCESS;
    }

    virtual XrResult GetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
                                                 VkPhysicalDevice* vulkanPhysicalDevice) override {
        PFN_xrGetVulkanGraphicsDeviceKHR pfnGetVulkanGraphicsDeviceKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDeviceKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsDeviceKHR)));

        if (getInfo->next != nullptr) {
            return XR_ERROR_FEATURE_UNSUPPORTED;
        }

        CHECK_XRCMD(pfnGetVulkanGraphicsDeviceKHR(instance, getInfo->systemId, getInfo->vulkanInstance, vulkanPhysicalDevice));

        return XR_SUCCESS;
    }

    virtual XrResult GetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId,
                                                       XrGraphicsRequirementsVulkan2KHR* graphicsRequirements) override {
        PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanGraphicsRequirementsKHR = nullptr;
        CHECK_XRCMD(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsRequirementsKHR)));

        XrGraphicsRequirementsVulkanKHR legacyRequirements{.type=XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR, .next=nullptr};
        CHECK_XRCMD(pfnGetVulkanGraphicsRequirementsKHR(instance, systemId, &legacyRequirements));

        graphicsRequirements->maxApiVersionSupported = legacyRequirements.maxApiVersionSupported;
        graphicsRequirements->minApiVersionSupported = legacyRequirements.minApiVersionSupported;

        return XR_SUCCESS;
    }
};

}  // namespace

std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_Vulkan(const std::shared_ptr<Options>& options,
                                                             std::shared_ptr<IPlatformPlugin> platformPlugin) {
    return std::make_shared<VulkanGraphicsPlugin>(options, std::move(platformPlugin));
}

std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_VulkanLegacy(const std::shared_ptr<Options>& options,
                                                                   std::shared_ptr<IPlatformPlugin> platformPlugin) {
    return std::make_shared<VulkanGraphicsPluginLegacy>(options, std::move(platformPlugin));
}

#endif  // XR_USE_GRAPHICS_API_VULKAN
