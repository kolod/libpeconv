#include "peconv/relocate.h"

#include "peconv/pe_hdrs_helper.h"
#include <iostream>
#include <stdio.h>

using namespace peconv;

#define RELOC_32BIT_FIELD 3
#define RELOC_64BIT_FIELD 0xA

typedef struct _BASE_RELOCATION_ENTRY {
    WORD Offset : 12;
    WORD Type : 4;
} BASE_RELOCATION_ENTRY;


class RelocBlockCallback
{
public:
    RelocBlockCallback(bool _is64bit)
        : is64bit(_is64bit)
    {
    }

    virtual bool processRelocField(ULONG_PTR relocField) = 0;

protected:
    bool is64bit;
};


class ApplyRelocCallback : public RelocBlockCallback
{
public:
    ApplyRelocCallback(bool _is64bit, ULONGLONG _oldBase, ULONGLONG _newBase)
        : RelocBlockCallback(_is64bit), oldBase(_oldBase), newBase(_newBase)
    {
    }

    virtual bool processRelocField(ULONG_PTR relocField)
    {
        if (is64bit) {
            ULONGLONG* relocateAddr = (ULONGLONG*)((ULONG_PTR)relocField);
            ULONGLONG rva = (*relocateAddr) - oldBase;
            (*relocateAddr) = rva + newBase;
        }
        else {
            DWORD* relocateAddr = (DWORD*)((ULONG_PTR)relocField);
            ULONGLONG rva = (*relocateAddr) - oldBase;
            (*relocateAddr) = static_cast<DWORD>(rva + newBase);
        }
        return true;
    }

protected:
    ULONGLONG oldBase;
    ULONGLONG newBase;
};

class IsRelocatedCallback : public RelocBlockCallback
{
public:
    IsRelocatedCallback(bool _is64bit, ULONGLONG _imageBase, DWORD _imageSize)
        : RelocBlockCallback(_is64bit), imageBase(_imageBase), imageSize(_imageSize)
    {
    }

    virtual bool processRelocField(ULONG_PTR relocField)
    {
        ULONGLONG rva = 0;
        if (is64bit) {
            ULONGLONG* relocateAddr = (ULONGLONG*)(relocField);
            rva = (*relocateAddr) - imageBase;
        }
        else {
            DWORD* relocateAddr = (DWORD*)(relocField);
            rva = (*relocateAddr) - imageBase;
        }
        if (rva > imageSize) {
            //std::cout << "RVA: " << std::hex << rva << " > imageSize: " << imageSize << "\n";
            return false;
        }
        return true;
    }
protected:
    ULONGLONG imageBase;
    DWORD imageSize;
};

bool process_reloc_block(BASE_RELOCATION_ENTRY *block, SIZE_T entriesNum, DWORD page, PVOID modulePtr, SIZE_T moduleSize, bool is64bit, RelocBlockCallback *callback)
{
    BASE_RELOCATION_ENTRY* entry = block;
    SIZE_T i = 0;
    for (i = 0; i < entriesNum; i++) {
        if (!validate_ptr(modulePtr, moduleSize, entry, sizeof(BASE_RELOCATION_ENTRY))) {
            break;
        }
        DWORD offset = entry->Offset;
        DWORD type = entry->Type;
        if (type == 0) {
            break;
        }
        if (type != RELOC_32BIT_FIELD && type != RELOC_64BIT_FIELD) {
            if (callback) { //print debug messages only if the callback function was set
                printf("[-] Not supported relocations format at %d: %d\n", (int)i, (int)type);
            }
            
            return false;
        }
        DWORD reloc_field = page + offset;
        if (reloc_field >= moduleSize) {
            if (callback) { //print debug messages only if the callback function was set
                printf("[-] Malformed field: %lx\n", reloc_field);
            }
            return false;
        }
        if (callback) {
            bool isOk = callback->processRelocField(((ULONG_PTR)modulePtr + reloc_field));
            if (!isOk) {
                //std::cout << "[-] Failed processing reloc field at: " << std::hex << reloc_field << "\n";
                return false;
            }
        }
        entry = (BASE_RELOCATION_ENTRY*)((ULONG_PTR)entry + sizeof(WORD));
    }
    return true;
}

bool process_relocation_table(PVOID modulePtr, SIZE_T moduleSize, RelocBlockCallback *callback)
{
    IMAGE_DATA_DIRECTORY* relocDir = peconv::get_directory_entry((const BYTE*)modulePtr, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (relocDir == NULL) {
#ifdef _DEBUG
        printf("[!] WARNING: no relocation table found!\n");
#endif
        return false;
    }
    if (!validate_ptr(modulePtr, moduleSize, relocDir, sizeof(IMAGE_DATA_DIRECTORY))) {
        return false;
    }
    DWORD maxSize = relocDir->Size;
    DWORD relocAddr = relocDir->VirtualAddress;
    bool is64b = is64bit((BYTE*)modulePtr);

    IMAGE_BASE_RELOCATION* reloc = NULL;

    DWORD parsedSize = 0;
    while (parsedSize < maxSize) {
        reloc = (IMAGE_BASE_RELOCATION*)(relocAddr + parsedSize + (ULONG_PTR)modulePtr);
        if (!validate_ptr(modulePtr, moduleSize, reloc, sizeof(IMAGE_BASE_RELOCATION))) {
            printf("[-] Invalid address of relocations\n");
            return false;
        }
        parsedSize += reloc->SizeOfBlock;

        if (reloc->SizeOfBlock == 0) {
            break;
        }

        size_t entriesNum = (reloc->SizeOfBlock - 2 * sizeof(DWORD)) / sizeof(WORD);
        DWORD page = reloc->VirtualAddress;

        BASE_RELOCATION_ENTRY* block = (BASE_RELOCATION_ENTRY*)((ULONG_PTR)reloc + sizeof(DWORD) + sizeof(DWORD));
        if (!validate_ptr(modulePtr, moduleSize, block, sizeof(BASE_RELOCATION_ENTRY))) {
            printf("[-] Invalid address of relocations block\n");
            return false;
        }
        if (process_reloc_block(block, entriesNum, page, modulePtr, moduleSize, is64b, callback) == false) {
            return false;
        }
    }
    return (parsedSize != 0);
}

bool apply_relocations(PVOID modulePtr, SIZE_T moduleSize, ULONGLONG newBase, ULONGLONG oldBase)
{
    const bool is64b = is64bit((BYTE*)modulePtr);
    ApplyRelocCallback callback(is64b, oldBase, newBase);
    return process_relocation_table(modulePtr, moduleSize, &callback);
}

bool peconv::relocate_module(IN BYTE* modulePtr, IN SIZE_T moduleSize, IN ULONGLONG newBase, IN ULONGLONG oldBase)
{
    if (modulePtr == NULL) {
        return false;
    }
    if (oldBase == 0) {
        oldBase = get_image_base(modulePtr);
    }
#ifdef _DEBUG
    printf("New Base: %llx\n", newBase);
    printf("Old Base: %llx\n", oldBase);
#endif
    if (newBase == oldBase) {
#ifdef _DEBUG
        printf("Nothing to relocate! oldBase is the same as the newBase!\n");
#endif
        return true; //nothing to relocate
    }
    if (!is_relocated_to_base(modulePtr, moduleSize, oldBase)) {
        if (is_relocated_to_base(modulePtr, moduleSize, newBase)) {
            // The module is already relocated to the newBase
            return true;
        }
        else {
            std::cout << "Could not relocate: the module is NOT relocated to the given oldBase: " << std::hex << oldBase << "\n";
            return false;
        }
    }
    if (apply_relocations(modulePtr, moduleSize, newBase, oldBase)) {
        return true;
    }
#ifdef _DEBUG
    printf("Could not relocate the module!\n");
#endif
    return false;
}

bool peconv::has_valid_relocation_table(IN const PBYTE modulePtr, IN const size_t moduleSize)
{
    return process_relocation_table(modulePtr, moduleSize, nullptr);
}

bool peconv::is_relocated_to_base(IN const PBYTE modulePtr, IN const size_t moduleSize, IN const ULONGLONG moduleBase)
{
    const bool is64b = is64bit((BYTE*)modulePtr);
    const DWORD image_size = peconv::get_image_size((BYTE*)modulePtr);
    IsRelocatedCallback callback(is64b, moduleBase, image_size);

    return process_relocation_table(modulePtr, moduleSize, &callback);
}
