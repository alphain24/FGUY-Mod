#pragma once
#include <windows.h>

// Replace one entry in a module's import address table. Returns false if the module does not
// import that function. On success *original receives the previous function pointer.
inline bool HookIat(HMODULE module, const char* dllName, const char* funcName, void* hook,
                    void** original) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dllName) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                // funcName may be an ordinal (MAKEINTRESOURCEA-style), e.g. for ws2_32 imports.
                if (!IS_INTRESOURCE(funcName) ||
                    IMAGE_ORDINAL(names->u1.Ordinal) != reinterpret_cast<ULONG_PTR>(funcName))
                    continue;
            } else {
                if (IS_INTRESOURCE(funcName)) continue;
                auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(byName->Name), funcName) != 0) continue;
            }

            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}
