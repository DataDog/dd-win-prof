// ObfSymbols.cpp - Extract symbols from PDB files and generate obfuscated output

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "PdbParser.h"

bool ParseCommandLine(int argc, wchar_t* argv[], std::wstring& pdbFile, std::wstring& outFile, std::wstring& obfFile, bool& dumpAll)
{
    dumpAll = false;

    for (int i = 1; i < argc; i++)
    {
        std::wstring arg = argv[i];

        if (arg == L"--pdb" && i + 1 < argc)
        {
            pdbFile = argv[++i];
        }
        else if (arg == L"--out" && i + 1 < argc)
        {
            outFile = argv[++i];
        }
        else if (arg == L"--obf" && i + 1 < argc)
        {
            obfFile = argv[++i];
        }
        else if (arg == L"--all")
        {
            dumpAll = true;
        }
        else
        {
            return false;
        }
    }

    // If --all flag is present, we only need pdb file
    if (dumpAll)
    {
        return !pdbFile.empty();
    }

    if (obfFile.empty() && !outFile.empty())
    {
        size_t dotPos = outFile.find_last_of(L'.');
        if (dotPos != std::wstring::npos)
        {
            obfFile = outFile.substr(0, dotPos) + L"_obf" + outFile.substr(dotPos);
        }
        else
        {
            obfFile = outFile + L"_obf";
        }
    }

    return !pdbFile.empty() && !outFile.empty();
}

std::wstring ObfuscateSymbolName(const std::wstring& originalName, size_t index)
{
    // Simple hash-based obfuscation using DJB2 algorithm
    unsigned long hash = 5381;
    for (wchar_t c : originalName)
    {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }

    // Mix in the index to ensure uniqueness
    hash = hash ^ (static_cast<unsigned long>(index) * 0x9e3779b9);

    wchar_t obfName[32];
    swprintf_s(obfName, 32, L"obf_%08lX", hash);

    return std::wstring(obfName);
}

void WriteModuleHeader(std::wofstream& stream, const ModuleInfo& moduleInfo)
{
    stream << L"MODULE " << moduleInfo.os << L" " << moduleInfo.architecture
           << L" " << moduleInfo.buildId << L" " << moduleInfo.moduleName << std::endl;
}

void WriteSymbolLine(std::wofstream& stream, const SymbolInfo& symbol, const std::wstring& obfuscatedName, bool includeSignature)
{
    stream << (symbol.isPublic ? L"PUBLIC" : L"PRIVATE")
           << L" " << std::hex << symbol.rva
           << L" " << std::hex << symbol.size
           << L" " << obfuscatedName;

    if (includeSignature)
    {
        stream << L" " << (!symbol.signature.empty() ? symbol.signature : symbol.name);

        if (symbol.conflictCount > 0)
        {
            stream << L" [CONFLICT " << (symbol.conflictCount + 1) << L"]";
        }
    }

    stream << std::endl;
}

bool WriteSymbolsToFileInternal(const std::wstring& outFile,
                                 const std::vector<SymbolInfo>& symbols,
                                 const ModuleInfo& moduleInfo,
                                 bool includeSignature)
{
    const std::wstring fileType = includeSignature ? L"" : L"obfuscated ";

    std::wofstream outStream(outFile);
    if (!outStream.is_open())
    {
        std::wcerr << L"Failed to open " << fileType << L"output file: " << outFile << std::endl;
        return false;
    }

    WriteModuleHeader(outStream, moduleInfo);

    for (size_t i = 0; i < symbols.size(); i++)
    {
        const auto& symbol = symbols[i];
        std::wstring obfuscatedName = ObfuscateSymbolName(symbol.name, i);
        WriteSymbolLine(outStream, symbol, obfuscatedName, includeSignature);
    }

    outStream.close();
    std::wcout << L"Successfully wrote " << symbols.size() << L" " << fileType << L"symbols to " << outFile << std::endl;

    return true;
}

bool WriteSymbolsToFile(const std::wstring& outFile, const std::vector<SymbolInfo>& symbols, const ModuleInfo& moduleInfo)
{
    return WriteSymbolsToFileInternal(outFile, symbols, moduleInfo, true);
}

bool WriteObfuscatedSymbolsToFile(const std::wstring& obfFile, const std::vector<SymbolInfo>& symbols, const ModuleInfo& moduleInfo)
{
    return WriteSymbolsToFileInternal(obfFile, symbols, moduleInfo, false);
}

bool ExtractSymbols(const std::wstring& pdbFile, const std::wstring& outFile, const std::wstring& obfFile)
{
    PdbParser parser(pdbFile);
    if (!parser.IsValid())
    {
        std::wcerr << L"Failed to initialize PDB parser or load PDB file" << std::endl;
        return false;
    }

    std::wcout << L"Extracting module information from PDB..." << std::endl;

    // Extract module information (GUID, architecture, module name)
    ModuleInfo moduleInfo;
    if (!parser.ExtractModuleInfo(moduleInfo))
    {
        std::wcerr << L"Failed to extract module information from PDB file" << std::endl;
        return false;
    }

    std::wcout << L"Module: " << moduleInfo.moduleName << std::endl;
    std::wcout << L"Architecture: " << moduleInfo.architecture << std::endl;
    std::wcout << L"Build ID: " << moduleInfo.buildId << std::endl;

    std::wcout << L"Extracting symbols from PDB using DIA SDK..." << std::endl;

    // Extract all function symbols using PDB parser
    std::vector<SymbolInfo> symbols;
    if (!parser.ExtractSymbols(symbols))
    {
        std::wcerr << L"Failed to extract symbols from PDB file" << std::endl;
        return false;
    }

    if (symbols.empty())
    {
        std::wcerr << L"No function symbols found in PDB file" << std::endl;
        return false;
    }

    bool success = WriteSymbolsToFile(outFile, symbols, moduleInfo);
    if (success)
    {
        success = WriteObfuscatedSymbolsToFile(obfFile, symbols, moduleInfo);
    }

    return success;
}

bool DumpAllSymbols(const std::wstring& pdbFile)
{
    PdbParser parser(pdbFile);
    if (!parser.IsValid())
    {
        std::wcerr << L"Failed to initialize PDB parser or load PDB file" << std::endl;
        return false;
    }

    std::wcout << L"Dumping all symbols from PDB..." << std::endl;

    if (!parser.DumpAllSymbols())
    {
        std::wcerr << L"Failed to dump symbols from PDB file" << std::endl;
        return false;
    }

    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring pdbFile;
    std::wstring outFile;
    std::wstring obfFile;
    bool dumpAll = false;

    if (!ParseCommandLine(argc, argv, pdbFile, outFile, obfFile, dumpAll))
    {
        std::wcerr << L"Usage: ObfSymbols --pdb <pdb_file> [--out <output_file>] [--obf <obfuscated_output_file>] [--all]" << std::endl;
        std::wcerr << L"  --all: Dump all symbols from PDB to console (no file output)" << std::endl;
        std::wcerr << L"  If --obf is not specified, the obfuscated file will be auto-generated" << std::endl;
        return 1;
    }

    std::wcout << L"PDB File: " << pdbFile << std::endl;

    if (dumpAll)
    {
        if (!DumpAllSymbols(pdbFile))
        {
            std::wcerr << L"Failed to dump all symbols" << std::endl;
            return 1;
        }
    }
    else
    {
        std::wcout << L"Output File: " << outFile << std::endl;
        std::wcout << L"Obfuscated Output File: " << obfFile << std::endl;

        if (!ExtractSymbols(pdbFile, outFile, obfFile))
        {
            std::wcerr << L"Failed to extract symbols" << std::endl;
            return 1;
        }
    }

    return 0;
}
