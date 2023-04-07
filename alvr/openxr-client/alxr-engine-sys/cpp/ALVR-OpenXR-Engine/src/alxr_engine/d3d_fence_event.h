#pragma once
#if defined(XR_USE_GRAPHICS_API_D3D12)
#include <atomic>
#include <d3d12.h>

struct D3D12FenceEvent {

    template < typename Type >
    using ComPtr = Microsoft::WRL::ComPtr<Type>;

    using ID3D12CommandQueuePtr = ComPtr<ID3D12CommandQueue>;

    ComPtr<ID3D12Fence>        fence{};
    std::atomic<std::uint64_t> fenceValue{ 0 };
    HANDLE                     fenceEvent = INVALID_HANDLE_VALUE;

    void CreateFence(const ComPtr<ID3D12Device>& device, const D3D12_FENCE_FLAGS fenceFlags = D3D12_FENCE_FLAG_NONE, LPCWSTR name = nullptr)
    {
        CHECK(device != nullptr);
        CHECK_HRCMD(device->CreateFence(fenceValue, fenceFlags,
            __uuidof(ID3D12Fence), reinterpret_cast<void**>(fence.ReleaseAndGetAddressOf())));
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        CHECK(fenceEvent != nullptr);
        if (name)
            SetName(name);
    }

    void SetName(LPCWSTR name) {
        fence->SetName(name);
    }

    bool IsComplete() const { return fenceValue.load() >= fence->GetCompletedValue(); }

    void WaitForGpu()
    {
        const std::uint64_t fenceVal = fenceValue.load();
        if (fence->GetCompletedValue() >= fenceVal)
            return;// true;
        CHECK_HRCMD(fence->SetEventOnCompletion(fenceVal, fenceEvent));
        const std::uint32_t retVal = WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
        //return retVal == WAIT_OBJECT_0;
        if (retVal != WAIT_OBJECT_0) {
            CHECK_HRCMD(E_FAIL);
        }
    }

    HRESULT Signal(const ID3D12CommandQueuePtr& cmdQueue) {
        ++fenceValue;
        return cmdQueue->Signal(fence.Get(), fenceValue.load());
        //fenceValue.store(nextValue);
    }

    HRESULT Wait(const ID3D12CommandQueuePtr& cmdQueue)
    {
        return cmdQueue->Wait(fence.Get(), fenceValue.load());
    }

    void SignalAndWaitForGpu(const ID3D12CommandQueuePtr& cmdQueue) {
        CHECK_HRCMD(Signal(cmdQueue));
        WaitForGpu();
    }
};
#endif

#if defined(XR_USE_GRAPHICS_API_D3D11)
#include <d3d11_4.h>

struct D3D11FenceEvent {

    template < typename Type >
    using ComPtr = Microsoft::WRL::ComPtr<Type>;

    ComPtr<ID3D11Fence>        fence{};
    std::atomic<std::uint64_t> fenceValue{ 0 };
    HANDLE                     fenceEvent = INVALID_HANDLE_VALUE;

    void CreateFence(const ComPtr<ID3D11Device5>& device, const D3D11_FENCE_FLAG fenceFlags = D3D11_FENCE_FLAG_NONE, LPCWSTR name = nullptr)
    {
        CHECK(device != nullptr);
        CHECK_HRCMD(device->CreateFence(fenceValue, fenceFlags,
            __uuidof(ID3D11Fence), reinterpret_cast<void**>(fence.ReleaseAndGetAddressOf())));
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        CHECK(fenceEvent != nullptr);
        //if (name)
        //    SetName(name);
    }

#if 0
    void SetName(LPCWSTR name) {
        fence->SetName(name);
    }
#endif

    bool IsComplete() const { return fenceValue.load() >= fence->GetCompletedValue(); }

    void WaitForGpu()
    {
        const std::uint64_t fenceVal = fenceValue.load();
        if (fence->GetCompletedValue() >= fenceVal)
            return;// true;
        CHECK_HRCMD(fence->SetEventOnCompletion(fenceVal, fenceEvent));
        const std::uint32_t retVal = WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
        //return retVal == WAIT_OBJECT_0;
        if (retVal != WAIT_OBJECT_0) {
            CHECK_HRCMD(E_FAIL);
        }
    }

#if 0
    HRESULT Signal(const ID3D12CommandQueuePtr& cmdQueue) {
        ++fenceValue;
        return cmdQueue->Signal(fence.Get(), fenceValue.load());
        //fenceValue.store(nextValue);
    }

    HRESULT Wait(const ID3D12CommandQueuePtr& cmdQueue)
    {
        return cmdQueue->Wait(fence.Get(), fenceValue.load());
    }

    void SignalAndWaitForGpu(const ID3D12CommandQueuePtr& cmdQueue) {
        CHECK_HRCMD(Signal(cmdQueue));
        WaitForGpu();
    }
#endif
};
#endif
