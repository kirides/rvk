#pragma once

// Development-only access to d3dx9_39.dll's CPU functions (shader assembler / disassembler) for tests and tools.
// d3dx9_39 ships with the DirectX runtime (and with Risen); nothing here touches a D3D9 device.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rvk::devtools {

struct D3DXBuffer {  // ID3DXBuffer
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
};

class D3DX {
public:
    using AssembleFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, D3DXBuffer**, D3DXBuffer**);
    using DisassembleFn = long(__stdcall*)(const DWORD*, BOOL, const char*, D3DXBuffer**);

    static const D3DX& get() {
        static const D3DX d = [] {
            D3DX x;
            if (HMODULE m = LoadLibraryW(L"d3dx9_39.dll")) {
                x.assemble_ = reinterpret_cast<AssembleFn>(GetProcAddress(m, "D3DXAssembleShader"));
                x.disassemble_ = reinterpret_cast<DisassembleFn>(GetProcAddress(m, "D3DXDisassembleShader"));
            }
            return x;
        }();
        return d;
    }
    bool available() const { return assemble_ != nullptr && disassemble_ != nullptr; }

    // D3D9 assembly -> bytecode tokens (empty on failure, `errors` gets the assembler output)
    std::vector<std::uint32_t> assemble(const std::string& src, std::string* errors = nullptr) const {
        D3DXBuffer* code = nullptr;
        D3DXBuffer* err = nullptr;
        const long hr = assemble_(src.c_str(), static_cast<UINT>(src.size()), nullptr, nullptr, 0, &code, &err);
        if (err != nullptr) {
            if (errors != nullptr) {
                errors->assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
            }
            err->Release();
        }
        if (hr < 0 || code == nullptr) {
            return {};
        }
        std::vector<std::uint32_t> tokens(code->GetBufferSize() / 4);
        std::memcpy(tokens.data(), code->GetBufferPointer(), tokens.size() * 4);
        code->Release();
        return tokens;
    }

    std::string disassemble(const std::vector<std::uint32_t>& tokens) const {
        D3DXBuffer* buf = nullptr;
        if (disassemble_(reinterpret_cast<const DWORD*>(tokens.data()), FALSE, nullptr, &buf) < 0 || buf == nullptr) {
            return {};
        }
        std::string s(static_cast<const char*>(buf->GetBufferPointer()), buf->GetBufferSize());
        buf->Release();
        return s;
    }

private:
    AssembleFn assemble_ = nullptr;
    DisassembleFn disassemble_ = nullptr;
};

}  // namespace rvk::devtools
