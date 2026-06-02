// COM export stubs — kept in a separate TU that does NOT include shobjidl.h /
// combaseapi.h so that DllGetClassObject and DllCanUnloadNow can be defined
// without conflicting with the __declspec(dllimport) declarations in those headers.
#include <windows.h>

HRESULT __stdcall WinzooCom_GetClassObject(const GUID& rclsid, const GUID& riid, void** ppv);
HRESULT __stdcall WinzooCom_CanUnload();

extern "C" HRESULT __stdcall DllGetClassObject(const GUID& rclsid, const GUID& riid, void** ppv) {
    return WinzooCom_GetClassObject(rclsid, riid, ppv);
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return WinzooCom_CanUnload();
}
