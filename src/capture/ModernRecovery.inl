// Included inside CaptureHooks.cpp's anonymous namespace. Discovery describes
// the loaded image's ABI; setter analysis supplies read-only access expressions.
// Neither component enumerates objects or runs a setter to discover its state.
struct ModernClass {
    modern::ClassInfo info;
    modern::ReadPlan input, execution, format, window;
};
struct ModernObject {
    std::shared_ptr<const ModernClass> type;
    std::string error;
    bool inputObserved{}, executionObserved{};
};
std::vector<std::shared_ptr<const ModernClass>> modernClasses;
std::map<void*, ModernObject> modernObjects;
std::set<HMODULE> modernModules;

void ForgetModernWeaver(void* key) { modernObjects.erase(key); }
bool ModernInputObserved(void* key) {
    std::lock_guard lock(controlMutex);
    if (auto it = modernObjects.find(key); it != modernObjects.end()) { it->second.inputObserved = true; return true; }
    return false;
}
bool ModernExecutionObserved(void* key) {
    std::lock_guard lock(controlMutex);
    if (auto it = modernObjects.find(key); it != modernObjects.end()) { it->second.executionObserved = true; return true; }
    return false;
}

HRESULT QueryRecovered(void* object, REFIID iid, void** out) noexcept {
    *out = nullptr;
    if (!object) return E_POINTER;
    // A decoded field must still identify a COM object of the expected type.
    // Protect only this validation boundary; do not swallow faults in rendering.
    __try { return static_cast<IUnknown*>(object)->QueryInterface(iid, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return E_NOINTERFACE; }
}

void InstallModernModule(HMODULE module) {
    if (!module || !modernModules.insert(module).second) return;
    std::string diagnostic;
    const auto types = modern::Discover(module, diagnostic);
    if (types.empty()) { Status("modern hot attach: " + diagnostic); return; }
    std::vector<std::shared_ptr<ModernClass>> additions;
    // Analyze every method before installing any detour, including methods
    // shared by multiple dynamic classes (compiler identical-code folding).
    for (const auto& info : types) {
        auto type = std::make_shared<ModernClass>(); type->info = info;
        type->input = modern::AnalyzeSetter(info.typed.vtable[info.dx12 ? leiaSlots->input12 : leiaSlots->input11]);
        type->execution = modern::AnalyzeSetter(info.typed.vtable[info.dx12 ? leiaSlots->list12 : leiaSlots->context11]);
        type->format = modern::AnalyzeSetter(info.typed.vtable[info.dx12 ? leiaSlots->input12 : leiaSlots->input11], 4, 4);
        type->window = modern::AnalyzeSetter(info.base.vtable[leiaSlots->window]);
        additions.push_back(std::move(type));
    }
    for (const auto& type : additions) {
        const auto& info = type->info;
        // These are precisely identified public-interface tables, not guesses
        // based on function size/order or the runtime's private class name.
        bool ok = Hook<WeaverWeave>(info.base.vtable[leiaSlots->weave]);
        ok &= Hook<WeaverWindow>(info.base.vtable[leiaSlots->window]);
        ok &= Hook<WeaverDestroy>(info.destroy.vtable[leiaSlots->destroy]);
        if (info.dx12) {
            ok &= Hook<Input12>(info.typed.vtable[leiaSlots->input12]);
            ok &= Hook<CommandList12>(info.typed.vtable[leiaSlots->list12]);
        } else {
            ok &= Hook<Input11>(info.typed.vtable[leiaSlots->input11]);
            ok &= Hook<Context11>(info.typed.vtable[leiaSlots->context11]);
        }
        if (!ok) Status("modern hot attach: could not hook all public methods for " + info.className);
        if (!type->input.valid || !type->execution.valid) {
            Status("modern hot attach: setter layout unsupported for " + info.className + ": " +
                (!type->input.valid ? type->input.diagnostic : type->execution.diagnostic));
        } else Status(std::string("modern hot attach ready: ") + (info.dx12 ? "DX12 " : "DX11 ") + info.className);
        modernClasses.push_back(type);
    }
}

void RecoverModernWeaver(SR::IWeaverBase1* self) {
    std::lock_guard lock(controlMutex);
    auto found = modernObjects.find(self);
    modern::ObjectViews views;
    if (found == modernObjects.end()) {
        // Factory-created objects already have authoritative input callbacks.
        if (Key(self)) return;
        for (const auto& type : modernClasses) {
            if (!modern::Locate(type->info, self, modern::InterfaceKind::Base, views)) continue;
            RegisterWeaver(views.base, nullptr, type->info.dx12);
            Bind(views.typed, views.base); Bind(views.base, views.base); Bind(views.destroy, views.base);
            found = modernObjects.emplace(views.base, ModernObject{type, {}}).first;
            Status(std::string("existing modern weaver observed: ") + (type->info.dx12 ? "DX12 " : "DX11 ") + type->info.className);
            break;
        }
        if (found == modernObjects.end()) return;
    } else if (!modern::Locate(found->second.type->info, self, modern::InterfaceKind::Base, views)) {
        // Refuse construction/destruction state and changed object identity.
        core->Input11(self, nullptr); core->Context11(self, nullptr);
        core->Input12(self, nullptr, DXGI_FORMAT_UNKNOWN); core->CommandList12(self, nullptr);
        return;
    }
    auto& object = found->second;
    const auto& type = *object.type;
    // A subsequently observed public setter is authoritative, including null.
    // Never let an unsupported old-field decoder undo a real setter callback.
    if (object.inputObserved && object.executionObserved) return;
    auto fail = [&](const std::string& reason) {
        if (type.info.dx12) {
            if (!object.inputObserved) core->Input12(views.base, nullptr, DXGI_FORMAT_UNKNOWN);
            if (!object.executionObserved) core->CommandList12(views.base, nullptr);
        } else {
            if (!object.inputObserved) core->Input11(views.base, nullptr);
            if (!object.executionObserved) core->Context11(views.base, nullptr);
        }
        if (object.error != reason) { object.error = reason; Status("modern hot attach: " + reason); }
    };
    uint64_t input{}, execution{}, format{}, window{};
    if ((!object.inputObserved && !type.input.valid) || (!object.executionObserved && !type.execution.valid)) {
        fail("cannot prove existing input/context layout: " + (!object.inputObserved && !type.input.valid ? type.input.diagnostic : type.execution.diagnostic)); return;
    }
    if ((!object.inputObserved && !type.input.Read(views.typed, input)) || (!object.executionObserved && !type.execution.Read(views.typed, execution))) {
        fail("existing input/context fields are unreadable or inconsistent"); return;
    }
    if ((!object.inputObserved && !input) || (!object.executionObserved && !execution)) { fail("waiting for the existing weaver's input and context/command list"); return; }
    if (type.window.valid && type.window.Read(views.base, window)) {
        const auto hwnd = reinterpret_cast<HWND>(window); DWORD pid{};
        if (hwnd && IsWindow(hwnd)) GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) core->Window(views.base, hwnd);
    }
    if (type.info.dx12) {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12GraphicsCommandList> list;
        if ((!object.inputObserved && FAILED(QueryRecovered(reinterpret_cast<void*>(input), IID_PPV_ARGS(&resource)))) ||
            (!object.executionObserved && FAILED(QueryRecovered(reinterpret_cast<void*>(execution), IID_PPV_ARGS(&list))))) {
            fail("decoded fields are not a DX12 resource and command list"); return;
        }
        ComPtr<ID3D12Device> resourceDevice, listDevice;
        if ((resource && FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice)))) || (list && FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice)))) ||
            (resource && list && resourceDevice.Get() != listDevice.Get()) || (list && list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)) {
            fail("existing DX12 resource/list devices or list type do not match"); return;
        }
        // If format recovery is unsupported, the resource's explicit format is
        // usable; typeless and unsupported formats are rejected by CaptureCore.
        auto viewFormat = resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
        if (resource && type.format.valid) {
            if (!type.format.Read(views.typed, format)) { fail("existing DX12 input format is inconsistent"); return; }
            viewFormat = static_cast<DXGI_FORMAT>(format);
        }
        AttachDevice(resourceDevice ? resourceDevice.Get() : listDevice.Get());
        if (list) core->CommandList12(views.base, AttachList(list.Get()) ? list.Get() : nullptr);
        if (resource) core->Input12(views.base, resource.Get(), viewFormat);
    } else {
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11DeviceContext> context;
        if ((!object.inputObserved && FAILED(QueryRecovered(reinterpret_cast<void*>(input), IID_PPV_ARGS(&view)))) ||
            (!object.executionObserved && FAILED(QueryRecovered(reinterpret_cast<void*>(execution), IID_PPV_ARGS(&context))))) {
            fail("decoded fields are not a DX11 SRV and context"); return;
        }
        ComPtr<ID3D11Device> viewDevice, contextDevice;
        if (view) view->GetDevice(&viewDevice);
        if (context) context->GetDevice(&contextDevice);
        if ((view && !viewDevice) || (context && !contextDevice) || (view && context && viewDevice.Get() != contextDevice.Get()) ||
            (context && context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)) {
            fail("existing DX11 input requires its matching immediate context"); return;
        }
        AttachLegacy11Device(views.base, viewDevice ? viewDevice.Get() : contextDevice.Get(), false);
        if (context) core->Context11(views.base, context.Get());
        if (view) core->Input11(views.base, view.Get());
    }
    object.error.clear();
}
