// PdbParser.cpp - Implementation of PDB parser using DIA SDK

#include "PdbParser.h"
#include "resource.h"
#include <iostream>
#include <algorithm>
#include <map>

#pragma comment(lib, "diaguids.lib")

PdbParser::PdbParser(const std::wstring& pdbFilePath)
    : _pdbFilePath(pdbFilePath)
    , _isValid(false)
    , _comInitialized(false)
    , _hasOMAP(false)
{
    _isValid = InitializeDiaAndLoadPdb();
    if (_isValid)
    {
        // Load OMAP tables if present
        LoadOMAPTables();
    }
}

PdbParser::~PdbParser()
{
    _pGlobal.Release();
    _pSession.Release();
    _pSource.Release();

    if (_comInitialized)
    {
        CoUninitialize();
    }
}

std::wstring PdbParser::GetExecutableDirectory()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring fullPath = path;
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
    {
        return fullPath.substr(0, lastSlash);
    }
    return L".";
}

bool PdbParser::ExtractEmbeddedDll(std::wstring& dllPath)
{
    std::wstring exeDir = GetExecutableDirectory();
    dllPath = exeDir + L"\\msdia140.dll";

    // Check if DLL already exists
    if (GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }

    HRSRC hResource = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_MSDIA140_DLL), RT_RCDATA);
    if (!hResource)
    {
        std::wcerr << L"Failed to find embedded DLL resource" << std::endl;
        return false;
    }

    HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
    if (!hLoadedResource)
    {
        std::wcerr << L"Failed to load embedded DLL resource" << std::endl;
        return false;
    }

    LPVOID pResourceData = LockResource(hLoadedResource);
    if (!pResourceData)
    {
        std::wcerr << L"Failed to lock embedded DLL resource" << std::endl;
        return false;
    }

    DWORD resourceSize = SizeofResource(NULL, hResource);
    if (resourceSize == 0)
    {
        std::wcerr << L"Embedded DLL resource has zero size" << std::endl;
        return false;
    }

    HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"Failed to create DLL file: " << dllPath << std::endl;
        std::wcerr << L"Error: " << GetLastError() << std::endl;
        return false;
    }

    DWORD bytesWritten;
    BOOL writeSuccess = WriteFile(hFile, pResourceData, resourceSize, &bytesWritten, NULL);
    CloseHandle(hFile);

    if (!writeSuccess || bytesWritten != resourceSize)
    {
        std::wcerr << L"Failed to write DLL file completely" << std::endl;
        DeleteFileW(dllPath.c_str());
        return false;
    }

    std::wcout << L"Extracted msdia140.dll to: " << dllPath << std::endl;
    return true;
}

bool PdbParser::InitializeDiaAndLoadPdb()
{
    HRESULT hr = CoInitialize(NULL);
    _comInitialized = SUCCEEDED(hr);

    // DIA could be registered on the machine
    hr = CoCreateInstance(__uuidof(DiaSource),
        NULL,
        CLSCTX_INPROC_SERVER,
        __uuidof(IDiaDataSource),
        (void**)&_pSource);

	// otherwise, try to load DIA from embedded/local msdia140.dll
    if (FAILED(hr))
    {
        std::wstring embeddedDllPath;
        if (!ExtractEmbeddedDll(embeddedDllPath))
        {
            std::wcerr << L"Failed to extract embedded msdia140.dll" << std::endl;
            return false;
        }

        HMODULE hDia = LoadLibraryW(embeddedDllPath.c_str());
        if (hDia == NULL)
        {
            std::wcerr << L"Failed to load msdia140.dll from: " << embeddedDllPath << std::endl;
            std::wcerr << L"Error code: " << GetLastError() << std::endl;
            return false;
        }

        typedef HRESULT(__stdcall* DllGetClassObjectFunc)(REFCLSID, REFIID, LPVOID*);
        DllGetClassObjectFunc pDllGetClassObject = (DllGetClassObjectFunc)GetProcAddress(hDia, "DllGetClassObject");

        if (pDllGetClassObject)
        {
            CComPtr<IClassFactory> pClassFactory;
            hr = pDllGetClassObject(__uuidof(DiaSource), IID_IClassFactory, (void**)&pClassFactory);
            if (SUCCEEDED(hr) && pClassFactory)
            {
                hr = pClassFactory->CreateInstance(NULL, __uuidof(IDiaDataSource), (void**)&_pSource);
            }
        }

        if (FAILED(hr) || !_pSource)
        {
            std::wcerr << L"Failed to create DIA source from embedded msdia140.dll" << std::endl;
            std::wcerr << L"Error code: " << std::hex << hr << std::endl;
            return false;
        }
    }

    hr = _pSource->loadDataFromPdb(_pdbFilePath.c_str());
    if (FAILED(hr))
    {
        std::wcerr << L"Failed to load PDB file: " << _pdbFilePath << std::endl;
        return false;
    }

    hr = _pSource->openSession(&_pSession);
    if (FAILED(hr) || !_pSession)
    {
        std::wcerr << L"Failed to open DIA session" << std::endl;
        return false;
    }

    hr = _pSession->get_globalScope(&_pGlobal);
    if (FAILED(hr) || !_pGlobal)
    {
        std::wcerr << L"Failed to get global scope" << std::endl;
        return false;
    }

    return true;
}

std::wstring PdbParser::GetArchitectureString(DWORD machineType)
{
    switch (machineType)
    {
    case 0x014c: // IMAGE_FILE_MACHINE_I386
        return L"x86";
    case 0x8664: // IMAGE_FILE_MACHINE_AMD64
        return L"x64";
    case 0xAA64: // IMAGE_FILE_MACHINE_ARM64
        return L"arm64";
    case 0x01c0: // IMAGE_FILE_MACHINE_ARM
        return L"arm";
    default:
        return L"unknown";
    }
}

std::wstring PdbParser::GetDiaTypeName(IDiaSymbol* pType)
{
    if (!pType)
        return L"void";

    BSTR bstrName = NULL;
    if (SUCCEEDED(pType->get_name(&bstrName)) && bstrName)
    {
        std::wstring result = bstrName;
        SysFreeString(bstrName);
        return result;
    }

    // If no name, check basic type
    DWORD baseType = 0;
    ULONGLONG length = 0;
    if (SUCCEEDED(pType->get_baseType(&baseType)))
    {
        pType->get_length(&length);

        switch (baseType)
        {
        case btVoid: return L"void";
        case btChar: return L"char";
        case btWChar: return L"wchar_t";
        case btInt:
        case btLong:
            if (length == 1) return L"char";
            if (length == 2) return L"short";
            if (length == 4) return L"int";
            if (length == 8) return L"__int64";
            return L"int";
        case btUInt:
        case btULong:
            if (length == 1) return L"unsigned char";
            if (length == 2) return L"unsigned short";
            if (length == 4) return L"unsigned int";
            if (length == 8) return L"unsigned __int64";
            return L"unsigned int";
        case btFloat:
            if (length == 4) return L"float";
            if (length == 8) return L"double";
            return L"float";
        case btBool: return L"bool";
        case btHresult: return L"HRESULT";
        default: return L"<unknown>";
        }
    }

    return L"<unknown>";
}

std::wstring PdbParser::GetDiaTypeString(IDiaSymbol* pType)
{
    if (!pType)
        return L"void";

    DWORD symTag = SymTagNull;
    if (FAILED(pType->get_symTag(&symTag)))
        return L"<unknown>";

    switch (symTag)
    {
    case SymTagPointerType:
    {
        CComPtr<IDiaSymbol> pBaseType;
        if (SUCCEEDED(pType->get_type(&pBaseType)) && pBaseType)
        {
            // Check if it's a reference
            BOOL isReference = FALSE;
            pType->get_reference(&isReference);

            std::wstring baseTypeName = GetDiaTypeString(pBaseType);
            if (isReference)
                return baseTypeName + L"&";
            else
                return baseTypeName + L"*";
        }
        return L"void*";
    }

    case SymTagArrayType:
    {
        CComPtr<IDiaSymbol> pBaseType;
        if (SUCCEEDED(pType->get_type(&pBaseType)) && pBaseType)
        {
            return GetDiaTypeString(pBaseType) + L"[]";
        }
        return L"<array>";
    }

    case SymTagBaseType:
    case SymTagUDT:
    case SymTagEnum:
    case SymTagTypedef:
        return GetDiaTypeName(pType);

    case SymTagFunctionType:
        return L"<function>";

    default:
        return GetDiaTypeName(pType);
    }
}

std::wstring PdbParser::GetDiaFunctionSignature(IDiaSymbol* pSymbol, const std::wstring& functionName)
{
    if (!pSymbol)
        return L"";

    std::wstring signature;

    CComPtr<IDiaSymbol> pFunctionType;
    if (FAILED(pSymbol->get_type(&pFunctionType)) || !pFunctionType)
    {
        return L"";
    }

    // Start with function name (no return type)
    signature = functionName + L"(";

    CComPtr<IDiaEnumSymbols> pEnumChildren;
    if (SUCCEEDED(pFunctionType->findChildren(SymTagFunctionArgType, NULL, nsNone, &pEnumChildren)) && pEnumChildren)
    {
        LONG count = 0;
        pEnumChildren->get_Count(&count);

        for (LONG i = 0; i < count; i++)
        {
            CComPtr<IDiaSymbol> pArg;
            ULONG celt = 0;
            if (SUCCEEDED(pEnumChildren->Next(1, &pArg, &celt)) && celt == 1 && pArg)
            {
                if (i > 0) signature += L", ";

                CComPtr<IDiaSymbol> pArgType;
                if (SUCCEEDED(pArg->get_type(&pArgType)) && pArgType)
                {
                    signature += GetDiaTypeString(pArgType);
                }
            }
        }
    }

    signature += L")";

    BOOL isConst = FALSE;
    if (SUCCEEDED(pFunctionType->get_constType(&isConst)) && isConst)
    {
        signature += L" const";
    }

    return signature;
}

bool PdbParser::ExtractModuleInfo(ModuleInfo& moduleInfo)
{
    if (!_isValid || !_pGlobal)
    {
        return false;
    }

    // Get GUID and age, concatenate them to form build ID
    GUID guid;
    DWORD age = 0;
    if (SUCCEEDED(_pGlobal->get_guid(&guid)))
    {
        _pGlobal->get_age(&age);

        wchar_t buildIdStr[80];
        swprintf_s(buildIdStr, 80, L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x%x",
            guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7],
            age);
        moduleInfo.buildId = buildIdStr;
    }

    DWORD machineType = 0;
    if (SUCCEEDED(_pGlobal->get_machineType(&machineType)))
    {
        moduleInfo.architecture = GetArchitectureString(machineType);
    }

    BSTR bstrName = NULL;
    if (SUCCEEDED(_pGlobal->get_name(&bstrName)) && bstrName)
    {
        moduleInfo.moduleName = bstrName;
        SysFreeString(bstrName);

        if (moduleInfo.moduleName.find(L'.') == std::wstring::npos)
        {
            // No extension - try to determine it from the PDB filename or filesystem
            size_t lastSlash = _pdbFilePath.find_last_of(L"\\/");
            size_t lastDot = _pdbFilePath.find_last_of(L'.');
            if (lastSlash != std::wstring::npos && lastDot != std::wstring::npos && lastDot > lastSlash)
            {
                std::wstring basePath = _pdbFilePath.substr(0, lastDot);
                if (GetFileAttributesW((basePath + L".dll").c_str()) != INVALID_FILE_ATTRIBUTES)
                    moduleInfo.moduleName += L".dll";
                else if (GetFileAttributesW((basePath + L".exe").c_str()) != INVALID_FILE_ATTRIBUTES)
                    moduleInfo.moduleName += L".exe";
                else
                    moduleInfo.moduleName += L".dll"; // Default assumption
            }
            else
            {
                moduleInfo.moduleName += L".dll"; // Default assumption
            }
        }
    }
    else
    {
        // Fallback: extract from PDB filename
        size_t lastSlash = _pdbFilePath.find_last_of(L"\\/");
        size_t lastDot = _pdbFilePath.find_last_of(L'.');
        if (lastSlash != std::wstring::npos && lastDot != std::wstring::npos && lastDot > lastSlash)
        {
            moduleInfo.moduleName = _pdbFilePath.substr(lastSlash + 1, lastDot - lastSlash - 1);
            // Try to determine extension from nearby files
            std::wstring basePath = _pdbFilePath.substr(0, lastDot);
            if (GetFileAttributesW((basePath + L".dll").c_str()) != INVALID_FILE_ATTRIBUTES)
                moduleInfo.moduleName += L".dll";
            else if (GetFileAttributesW((basePath + L".exe").c_str()) != INVALID_FILE_ATTRIBUTES)
                moduleInfo.moduleName += L".exe";
            else
                moduleInfo.moduleName += L".dll"; // Default assumption
        }
        else
        {
            moduleInfo.moduleName = L"unknown.dll";
        }
    }

    std::wstring detectedOS = L"windows";

    // Detect OS from PDB by examining source file paths
    // Try to enumerate source files to detect path style
    CComPtr<IDiaEnumSourceFiles> pEnumSourceFiles;
    if (SUCCEEDED(_pSession->findFile(NULL, NULL, nsNone, &pEnumSourceFiles)) && pEnumSourceFiles)
    {
        CComPtr<IDiaSourceFile> pSourceFile;
        ULONG celt = 0;
        int windowsPaths = 0;
        int unixPaths = 0;
        int filesChecked = 0;

        while (filesChecked < 10 && SUCCEEDED(pEnumSourceFiles->Next(1, &pSourceFile, &celt)) && celt == 1 && pSourceFile)
        {
            BSTR bstrFileName = NULL;
            if (SUCCEEDED(pSourceFile->get_fileName(&bstrFileName)) && bstrFileName)
            {
                std::wstring fileName = bstrFileName;
                SysFreeString(bstrFileName);

                // Check path style
                if (fileName.find(L'\\') != std::wstring::npos)
                    windowsPaths++;
                if (fileName.find(L'/') != std::wstring::npos)
                {
                    // Check if it's a Unix-style absolute path
                    if (fileName.length() > 0 && fileName[0] == L'/')
                        unixPaths++;
                }

                filesChecked++;
            }
            pSourceFile.Release();
        }

        // Determine OS based on path style
        if (unixPaths > 0 && unixPaths >= windowsPaths)
        {
            // Unix-style paths detected - check for Mac-specific paths
            bool hasMacPath = false;
            pEnumSourceFiles->Reset();
            filesChecked = 0;
            while (filesChecked < 10 && SUCCEEDED(pEnumSourceFiles->Next(1, &pSourceFile, &celt)) && celt == 1 && pSourceFile)
            {
                BSTR bstrFileName = NULL;
                if (SUCCEEDED(pSourceFile->get_fileName(&bstrFileName)) && bstrFileName)
                {
                    std::wstring fileName = bstrFileName;
                    SysFreeString(bstrFileName);
                    if (fileName.find(L"/Users/") != std::wstring::npos ||
                        fileName.find(L"/System/") != std::wstring::npos ||
                        fileName.find(L"/Library/") != std::wstring::npos)
                    {
                        hasMacPath = true;
                        break;
                    }
                    filesChecked++;
                }
                pSourceFile.Release();
            }

            detectedOS = hasMacPath ? L"mac" : L"linux";
        }
        else if (windowsPaths > 0)
        {
            detectedOS = L"windows";
        }
    }

    moduleInfo.os = detectedOS;

    return true;
}

// Load OMAP tables from debug streams
bool PdbParser::LoadOMAPTables()
{
    if (!_pSession)
        return false;

    _hasOMAP = false;
    _omapFrom.clear();
    _omapTo.clear();

    CComPtr<IDiaEnumDebugStreams> pEnumStreams;
    HRESULT hr = _pSession->getEnumDebugStreams(&pEnumStreams);
    if (SUCCEEDED(hr) && pEnumStreams)
    {
        CComPtr<IDiaEnumDebugStreamData> pStream;
        ULONG celt = 0;
        while (SUCCEEDED(pEnumStreams->Next(1, &pStream, &celt)) && celt == 1 && pStream)
        {
            BSTR bstrName = NULL;
            if (SUCCEEDED(pStream->get_name(&bstrName)) && bstrName)
            {
                std::wstring streamName = bstrName;
                SysFreeString(bstrName);

                LONG count = 0;
                pStream->get_Count(&count);

                if (streamName == L"OMAPTO" && count > 0)
                {
                    // Read OMAPTO entries (optimized -> original)
                    _omapTo.resize(count);
                    DWORD cbData = 0;
                    hr = pStream->Next(count, sizeof(OMAPRVA), &cbData, (BYTE*)_omapTo.data(), NULL);
                    if (SUCCEEDED(hr))
                    {
                        std::wcout << L"Loaded OMAPTO table with " << count << L" entries" << std::endl;
                    }
                }
                else if (streamName == L"OMAPFROM" && count > 0)
                {
                    // Read OMAPFROM entries (original -> optimized)
                    _omapFrom.resize(count);
                    DWORD cbData = 0;
                    hr = pStream->Next(count, sizeof(OMAPRVA), &cbData, (BYTE*)_omapFrom.data(), NULL);
                    if (SUCCEEDED(hr))
                    {
                        std::wcout << L"Loaded OMAPFROM table with " << count << L" entries" << std::endl;
                    }
                }
            }
            pStream.Release();
        }
    }

    _hasOMAP = !_omapFrom.empty() && !_omapTo.empty();
    if (_hasOMAP)
    {
        std::wcout << L"OMAP tables loaded successfully - this is an optimized binary" << std::endl;
    }
    else
    {
        std::wcout << L"No OMAP tables found - this is a non-optimized binary" << std::endl;
    }

    return _hasOMAP;
}

// Translate RVA using OMAPFROM table (original -> optimized)
DWORD PdbParser::TranslateRVAFromOriginal(DWORD rvaOriginal) const
{
    if (!_hasOMAP || _omapFrom.empty())
        return rvaOriginal;

    // Binary search for the entry with rva <= rvaOriginal
    // OMAP tables are sorted by rva
    size_t left = 0;
    size_t right = _omapFrom.size();

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        if (_omapFrom[mid].rva <= rvaOriginal)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    // left now points to the first entry with rva > rvaOriginal
    // so we want the entry before it
    if (left == 0)
        return rvaOriginal;  // No mapping found

    size_t idx = left - 1;
    const OMAPRVA& entry = _omapFrom[idx];

    // Check for special case: rvaTo == 0 means the code was eliminated
    if (entry.rvaTo == 0)
        return 0;

    // Calculate offset from the entry and apply to target
    DWORD offset = rvaOriginal - entry.rva;
    return entry.rvaTo + offset;
}

// Translate RVA using OMAPTO table (optimized -> original)
DWORD PdbParser::TranslateRVAToOriginal(DWORD rvaOptimized) const
{
    if (!_hasOMAP || _omapTo.empty())
        return rvaOptimized;

    // Binary search for the entry with rva <= rvaOptimized
    size_t left = 0;
    size_t right = _omapTo.size();

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        if (_omapTo[mid].rva <= rvaOptimized)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    if (left == 0)
        return rvaOptimized;

    size_t idx = left - 1;
    const OMAPRVA& entry = _omapTo[idx];

    if (entry.rvaTo == 0)
        return 0;

    DWORD offset = rvaOptimized - entry.rva;
    return entry.rvaTo + offset;
}

ULONG PdbParser::CalculateSizeWithOMAP(DWORD rvaOriginal, ULONGLONG sizeOriginal) const
{
    if (!_hasOMAP || sizeOriginal == 0)
        return static_cast<ULONG>(sizeOriginal);

    DWORD rvaOptimizedStart = TranslateRVAFromOriginal(rvaOriginal);
    DWORD rvaOriginalEnd = rvaOriginal + static_cast<DWORD>(sizeOriginal);
    DWORD rvaOptimizedEnd = TranslateRVAFromOriginal(rvaOriginalEnd);

    // Check for eliminated code
    if (rvaOptimizedStart == 0 || rvaOptimizedEnd == 0)
        return 0;

    // Calculate the new size
    // Note: Due to optimization, code may be non-contiguous, so we take the difference
    if (rvaOptimizedEnd > rvaOptimizedStart)
    {
        return rvaOptimizedEnd - rvaOptimizedStart;
    }
    else
    {
        // Code was rearranged and is no longer contiguous
        // In this case, we can't accurately determine the size
        // Return the original size as a best effort
        return static_cast<ULONG>(sizeOriginal);
    }
}

bool PdbParser::ExtractSymbols(std::vector<SymbolInfo>& symbols)
{
    if (!_isValid || !_pGlobal)
    {
        return false;
    }

    // Enumerate all function symbols by passing SymTagFunction
    CComPtr<IDiaEnumSymbols> pEnumSymbols;
    HRESULT hr = _pGlobal->findChildren(SymTagFunction, NULL, nsNone, &pEnumSymbols);
    if (FAILED(hr) || !pEnumSymbols)
    {
        return false;
    }

    // Use a map to deduplicate symbols by translated RVA
    // This also handles OMAP translations from optimized code
    std::map<DWORD, SymbolInfo> symbolMap;

    CComPtr<IDiaSymbol> pSymbol;
    ULONG celt = 0;
    int skippedSymbols = 0;

    while (SUCCEEDED(pEnumSymbols->Next(1, &pSymbol, &celt)) && celt == 1 && pSymbol)
    {
        SymbolInfo info;

        DWORD rvaOriginal = 0;
        if (FAILED(pSymbol->get_relativeVirtualAddress(&rvaOriginal)))
        {
            pSymbol.Release();
            continue;
        }

        ULONGLONG sizeOriginal = 0;
        pSymbol->get_length(&sizeOriginal);

        // Explicitly translate RVA and size using OMAP tables
        DWORD rvaOptimized = 0;
        ULONG sizeOptimized = 0;

        if (_hasOMAP)
        {
            rvaOptimized = TranslateRVAFromOriginal(rvaOriginal);

            // Check if code was eliminated during optimization
            if (rvaOptimized == 0)
            {
                skippedSymbols++;
                pSymbol.Release();
                continue;
            }

            sizeOptimized = CalculateSizeWithOMAP(rvaOriginal, sizeOriginal);

            // Skip symbols with zero size after optimization
            if (sizeOptimized == 0)
            {
                skippedSymbols++;
                pSymbol.Release();
                continue;
            }
        }
        else
        {
            rvaOptimized = rvaOriginal;
            sizeOptimized = static_cast<ULONG>(sizeOriginal);
        }

        info.rva = rvaOptimized;
        info.size = sizeOptimized;

        BSTR bstrName = NULL;
        if (FAILED(pSymbol->get_name(&bstrName)) || !bstrName)
        {
            pSymbol.Release();
            continue;
        }
        info.name = bstrName;
        SysFreeString(bstrName);

        info.signature = GetDiaFunctionSignature(pSymbol, info.name);

        // Check if symbol is exported (public)
        DWORD locationType = 0;
        if (SUCCEEDED(pSymbol->get_locationType(&locationType)))
        {
            // LocIsExport (2) = external/exported
            info.isPublic = (locationType == 2);  // LocIsExport
        }
        else
        {
            // If we can't determine, assume private
            info.isPublic = false;
        }

        info.conflictCount = 0;

        // Check if this RVA already has a symbol
        auto it = symbolMap.find(rvaOptimized);
        if (it != symbolMap.end())
        {
            // Multiple symbols at same RVA - we have a conflict
            // conflictCount represents number of OTHER symbols at this RVA
            WORD newConflictCount = it->second.conflictCount + 1;

            // Keep the lexically smaller name
            if (info.name < it->second.name)
            {
                info.conflictCount = newConflictCount;
                symbolMap[rvaOptimized] = info;
            }
            else
            {
                it->second.conflictCount = newConflictCount;
            }
        }
        else
        {
            symbolMap[rvaOptimized] = info;
        }

        pSymbol.Release();
    }

    // Convert map to vector (already sorted by RVA due to map ordering)
    symbols.reserve(symbolMap.size());
    int rvasWithConflicts = 0;
    int totalConflictingSymbols = 0;
    for (const auto& pair : symbolMap)
    {
        symbols.push_back(pair.second);
        if (pair.second.conflictCount > 0)
        {
            rvasWithConflicts++;
            // Total symbols = conflictCount (other symbols) + 1 (this symbol)
            totalConflictingSymbols += pair.second.conflictCount + 1;
        }
    }

    std::wcout << L"Successfully extracted " << symbols.size() << L" function symbols";
    if (skippedSymbols > 0)
    {
        std::wcout << L" (skipped " << skippedSymbols << L" eliminated symbols)";
    }
    if (rvasWithConflicts > 0)
    {
        std::wcout << L" (" << rvasWithConflicts << L" RVAs with conflicts, "
                   << totalConflictingSymbols << L" total symbols)";
    }
    std::wcout << std::endl;

    return true;
}
