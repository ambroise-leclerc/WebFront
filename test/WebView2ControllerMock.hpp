#pragma once

#include <frontend/WebView2.hpp>

#include <functional>

namespace webfront::test {

// Only window integration is exercised; unused browser services are deliberately unavailable.
class WebView2ControllerMock : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICoreWebView2Controller> {
public:
    unsigned                       focusCalls     = 0;
    unsigned                       closeCalls     = 0;
    HRESULT                        closeResult    = S_OK;
    unsigned                       positionCalls  = 0;
    HRESULT                        focusResult    = S_OK;
    HRESULT                        positionResult = S_OK;
    std::function<void()>          duringClose;
    COREWEBVIEW2_MOVE_FOCUS_REASON focusReason = COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT;

    HRESULT STDMETHODCALLTYPE MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON reason) override {
        ++focusCalls;
        focusReason = reason;
        return focusResult;
    }
    HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() override {
        ++positionCalls;
        return positionResult;
    }
    HRESULT STDMETHODCALLTYPE Close() override {
        ++closeCalls;
        if (duringClose)
            duringClose();
        return closeResult;
    }
    HRESULT STDMETHODCALLTYPE put_Bounds(RECT) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_Bounds(RECT*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_ZoomFactor(double*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_ZoomFactor(double) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged(ICoreWebView2ZoomFactorChangedEventHandler*, EventRegistrationToken*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE remove_ZoomFactorChanged(EventRegistrationToken) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetBoundsAndZoomFactor(RECT, double) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_MoveFocusRequested(ICoreWebView2MoveFocusRequestedEventHandler*, EventRegistrationToken*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE remove_MoveFocusRequested(EventRegistrationToken) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_GotFocus(ICoreWebView2FocusChangedEventHandler*, EventRegistrationToken*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE remove_GotFocus(EventRegistrationToken) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_LostFocus(ICoreWebView2FocusChangedEventHandler*, EventRegistrationToken*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE remove_LostFocus(EventRegistrationToken) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed(ICoreWebView2AcceleratorKeyPressedEventHandler*, EventRegistrationToken*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE remove_AcceleratorKeyPressed(EventRegistrationToken) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_ParentWindow(HWND*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_ParentWindow(HWND) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_CoreWebView2(ICoreWebView2**) override {
        return E_NOTIMPL;
    }
};

}  // namespace webfront::test
