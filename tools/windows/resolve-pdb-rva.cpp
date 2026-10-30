// Diagnostic-only tool: resolve a fault RVA using the *exact* matching Release PDB.
// Never ships inside the Agent or runs on a customer machine.
#define NOMINMAX
#include <windows.h>
#include <dia2.h>
#include <cstdio>
#include <string>
#include <initializer_list>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "diaguids.lib")
static std::string narrow(const wchar_t* wide) {
    if (!wide) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), n, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(h)) { printf("COM_INIT_FAILED=0x%08X\n", (unsigned)h); return 3; }
    IDiaDataSource* source = nullptr;
    h = CoCreateInstance(CLSID_DiaSource, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IDiaDataSource, reinterpret_cast<void**>(&source));
    printf("DIA_CREATE=0x%08X\n", (unsigned)h);
    if (FAILED(h)) {
        HMODULE module = LoadLibraryW(argv[2]);
        if (!module) { printf("DIA_LOAD_DLL_FAILED=%lu\n", GetLastError()); return 4; }
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*)>(
            GetProcAddress(module, "DllGetClassObject"));
        IClassFactory* factory = nullptr;
        h = getClass ? getClass(CLSID_DiaSource, IID_IClassFactory,
                                  reinterpret_cast<void**>(&factory)) : E_FAIL;
        if (SUCCEEDED(h)) {
            h = factory->CreateInstance(nullptr, IID_IDiaDataSource,
                                        reinterpret_cast<void**>(&source));
            factory->Release();
        }
        printf("DIA_CREATE_FROM_DLL=0x%08X\n", (unsigned)h);
        if (FAILED(h)) return 5;
    }
    h = source->loadDataFromPdb(argv[1]);
    printf("PDB_LOAD=0x%08X\n", (unsigned)h);
    if (FAILED(h)) return 6;
    IDiaSession* session = nullptr;
    h = source->openSession(&session);
    if (FAILED(h)) { printf("DIA_SESSION_FAILED=0x%08X\n", (unsigned)h); return 7; }

    const DWORD offsets[] = { 0x31C8D0, 0x31C8E0, 0x31C8E7,
                              0x31C8EB, 0x31C8EF, 0x31C8F3, 0x31C900 };
    for (DWORD rva : offsets) {
        printf("DIA_RVA=0x%08X\n", rva);
        for (auto tag : { SymTagFunction, SymTagPublicSymbol, SymTagNull }) {
            IDiaSymbol* symbol = nullptr;
            h = session->findSymbolByRVA(rva, tag, &symbol);
            printf("  TAG=%u FOUND=%d HR=0x%08X",
                   (unsigned)tag, symbol != nullptr, (unsigned)h);
            if (symbol) {
                BSTR name = nullptr;
                if (SUCCEEDED(symbol->get_name(&name)) && name) {
                    printf(" NAME=%s", narrow(name).c_str());
                    SysFreeString(name);
                }
                DWORD start = 0;
                if (SUCCEEDED(symbol->get_relativeVirtualAddress(&start)))
                    printf(" SYMBOL_RVA=0x%08X", start);
                ULONGLONG size = 0;
                if (SUCCEEDED(symbol->get_length(&size)))
                    printf(" SIZE=%llu", (unsigned long long)size);
                symbol->Release();
            }
            printf("\n");
        }
        IDiaEnumLineNumbers* lines = nullptr;
        h = session->findLinesByRVA(rva, 1, &lines);
        printf("  LINE_LOOKUP_HR=0x%08X\n", (unsigned)h);
        if (SUCCEEDED(h) && lines) {
            IDiaLineNumber* line = nullptr;
            ULONG fetched = 0;
            if (SUCCEEDED(lines->Next(1, &line, &fetched)) && fetched == 1) {
                DWORD number = 0;
                line->get_lineNumber(&number);
                IDiaSourceFile* file = nullptr;
                if (SUCCEEDED(line->get_sourceFile(&file)) && file) {
                    BSTR fileName = nullptr;
                    if (SUCCEEDED(file->get_fileName(&fileName)) && fileName) {
                        printf("  LOCATION=%s:%lu\n", narrow(fileName).c_str(), number);
                        SysFreeString(fileName);
                    }
                    file->Release();
                }
                line->Release();
            }
            lines->Release();
        }
    }
    session->Release();
    source->Release();
    CoUninitialize();
    return 0;
}
