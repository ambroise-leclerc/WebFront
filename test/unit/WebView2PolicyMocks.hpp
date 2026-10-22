#pragma once
#include <webfront/frontend/WebView2.hpp>

#include <cstring>

namespace webfront::test {

class WebView2SettingsMock : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICoreWebView2Settings> {
public:
    BOOL                      devTools       = TRUE;
    BOOL                      contextMenus   = TRUE;
    HRESULT                   settingsResult = S_OK;
    HRESULT STDMETHODCALLTYPE get_AreDevToolsEnabled(BOOL* settingValue) override {
        *settingValue = devTools;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AreDevToolsEnabled(BOOL settingValue) override {
        devTools = settingValue;
        return settingsResult;
    }
    HRESULT STDMETHODCALLTYPE get_AreDefaultContextMenusEnabled(BOOL* settingValue) override {
        *settingValue = contextMenus;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AreDefaultContextMenusEnabled(BOOL settingValue) override {
        contextMenus = settingValue;
        return settingsResult;
    }
    HRESULT STDMETHODCALLTYPE get_IsScriptEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsScriptEnabled(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_IsWebMessageEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsWebMessageEnabled(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_AreDefaultScriptDialogsEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_AreDefaultScriptDialogsEnabled(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_IsStatusBarEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsStatusBarEnabled(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_AreHostObjectsAllowed(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_AreHostObjectsAllowed(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_IsZoomControlEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsZoomControlEnabled(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_IsBuiltInErrorPageEnabled(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsBuiltInErrorPageEnabled(BOOL) override {
        return E_NOTIMPL;
    }
};

class WebView2PopupMock
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICoreWebView2NewWindowRequestedEventArgs> {
public:
    BOOL                      handled             = FALSE;
    HRESULT                   handledResult       = S_OK;
    HRESULT                   uriResult           = S_OK;
    BOOL                      userInitiated       = TRUE;
    HRESULT                   userInitiatedResult = S_OK;
    HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* settingValue) override {
        if (FAILED(uriResult))
            return uriResult;
        constexpr wchar_t uri[] = L"https://example.com/help";
        *settingValue           = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(uri)));
        if (!*settingValue)
            return E_OUTOFMEMORY;
        std::memcpy(*settingValue, uri, sizeof(uri));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Handled(BOOL settingValue) override {
        handled = settingValue;
        return handledResult;
    }
    HRESULT STDMETHODCALLTYPE get_Handled(BOOL* settingValue) override {
        *settingValue = handled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_NewWindow(ICoreWebView2**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_NewWindow(ICoreWebView2*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL* settingValue) override {
        *settingValue = userInitiated;
        return userInitiatedResult;
    }
    HRESULT STDMETHODCALLTYPE get_WindowFeatures(ICoreWebView2WindowFeatures**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDeferral(ICoreWebView2Deferral**) override {
        return E_NOTIMPL;
    }
};
}  // namespace webfront::test
