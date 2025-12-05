// ObfSymbols.cpp - Extract symbols from PDB files and generate obfuscated output
// Uses PdbParser class for PDB analysis via DIA SDK

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "PdbParser.h"

// Helper function to parse command line arguments
bool ParseCommandLine(int argc, wchar_t* argv[], std::wstring& pdbFile, std::wstring& outFile, std::wstring& obfFile)
{
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
        else
        {
            return false;
        }
    }

    // If obfFile is not provided, generate it from outFile
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

// Simple hash function to generate obfuscated symbol names
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

    // Generate obfuscated name in format: obf_XXXXXXXX
    wchar_t obfName[32];
    swprintf_s(obfName, 32, L"obf_%08lX", hash);

    return std::wstring(obfName);
}

// Helper function to write MODULE header
void WriteModuleHeader(std::wofstream& stream, const ModuleInfo& moduleInfo)
{
    stream << L"MODULE " << moduleInfo.os << L" " << moduleInfo.architecture
           << L" " << moduleInfo.buildId << L" " << moduleInfo.moduleName << std::endl;
}

// Helper function to write a single symbol line
void WriteSymbolLine(std::wofstream& stream, const SymbolInfo& symbol, const std::wstring& obfuscatedName, bool includeSignature)
{
    stream << (symbol.isPublic ? L"PUBLIC" : L"PRIVATE")
           << L" " << std::hex << symbol.rva
           << L" " << std::hex << symbol.size
           << L" " << obfuscatedName;

    if (includeSignature)
    {
        // Include signature or fall back to symbol name
        stream << L" " << (!symbol.signature.empty() ? symbol.signature : symbol.name);

        // Add conflict marker if multiple symbols mapped to same RVA
        if (symbol.conflictCount > 0)
        {
            // Display total number of symbols at this RVA (conflictCount + 1)
            stream << L" [CONFLICT " << (symbol.conflictCount + 1) << L"]";
        }
    }

    stream << std::endl;
}

// Generic function to write symbols to output file
bool WriteSymbolsToFileInternal(const std::wstring& outFile,
                                 const std::vector<SymbolInfo>& symbols,
                                 const ModuleInfo& moduleInfo,
                                 bool includeSignature)
{
    // Infer file type from includeSignature
    const std::wstring fileType = includeSignature ? L"" : L"obfuscated ";

    std::wofstream outStream(outFile);
    if (!outStream.is_open())
    {
        std::wcerr << L"Failed to open " << fileType << L"output file: " << outFile << std::endl;
        return false;
    }

    // Write MODULE header
    WriteModuleHeader(outStream, moduleInfo);

    // Write symbols
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

// Write symbols to output file with signatures (readable mapping file)
bool WriteSymbolsToFile(const std::wstring& outFile, const std::vector<SymbolInfo>& symbols, const ModuleInfo& moduleInfo)
{
    return WriteSymbolsToFileInternal(outFile, symbols, moduleInfo, true);
}

// Write obfuscated symbols to output file (fully obfuscated, no signatures)
bool WriteObfuscatedSymbolsToFile(const std::wstring& obfFile, const std::vector<SymbolInfo>& symbols, const ModuleInfo& moduleInfo)
{
    return WriteSymbolsToFileInternal(obfFile, symbols, moduleInfo, false);
}

// Main function to extract symbols from PDB file and write to output files
bool ExtractSymbols(const std::wstring& pdbFile, const std::wstring& outFile, const std::wstring& obfFile)
{
    // Create PDB parser (initializes COM and loads PDB)
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

    // Write symbols to file
    bool success = WriteSymbolsToFile(outFile, symbols, moduleInfo);

    // Write obfuscated symbols to file
    if (success)
    {
        success = WriteObfuscatedSymbolsToFile(obfFile, symbols, moduleInfo);
    }

    return success;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring pdbFile;
    std::wstring outFile;
    std::wstring obfFile;

    if (!ParseCommandLine(argc, argv, pdbFile, outFile, obfFile))
    {
        std::wcerr << L"Usage: ObfSymbols --pdb <pdb_file> --out <output_file> [--obf <obfuscated_output_file>]" << std::endl;
        std::wcerr << L"  If --obf is not specified, the obfuscated file will be auto-generated" << std::endl;
        return 1;
    }

    std::wcout << L"PDB File: " << pdbFile << std::endl;
    std::wcout << L"Output File: " << outFile << std::endl;
    std::wcout << L"Obfuscated Output File: " << obfFile << std::endl;

    if (!ExtractSymbols(pdbFile, outFile, obfFile))
    {
        std::wcerr << L"Failed to extract symbols" << std::endl;
        return 1;
    }

    return 0;
}
