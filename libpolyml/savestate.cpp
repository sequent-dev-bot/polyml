/*
    Title:  savestate.cpp - Save and Load state

    Copyright (c) 2007, 2015, 2017-19, 2021, 2025-6 David C.J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/


#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h> // For MAX_PATH
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h> // For MAX_PATH
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <stdexcept>
#include <chrono>
#include <ctime>

#if (defined(_WIN32))
#include <tchar.h>
#define ERRORNUMBER _doserrno
#define NOMEMORY ERROR_NOT_ENOUGH_MEMORY
#ifdef _UNICODE
#define std_tstring std::wstring
#else
#define std_tstring std::string
#endif
#else
typedef char TCHAR;
#define _T(x) x
#define _tfopen fopen
#define _tcscpy strcpy
#define _tcsdup strdup
#define _tcslen strlen
#define _fputtc fputc
#define _fputts fputs
#ifndef lstrcmpi
#define lstrcmpi strcasecmp
#endif
#define ERRORNUMBER errno
#define NOMEMORY ENOMEM
#define std_tstring std::string
#endif

#include "globals.h"
#include "savestate.h"
#include "processes.h"
#include "run_time.h"
#include "polystring.h"
#include "scanaddrs.h"
#include "arb.h"
#include "memmgr.h"
#include "mpoly.h" // For exportSignature
#include "exporter.h" // For CopyScan
#include "machine_dep.h"
#include "osmem.h"
#include "gc.h" // For FullGC.
#include "timing.h"
#include "rtsentry.h"
#include "check_objects.h"
#include "rtsentry.h"
#include "timing.h" // For getBuildTime
#include "exporter.h" // For CopyScan
#include "savestate.h"

#include "mpoly.h" // For exportSignature

#include "../polyexports.h"

#define SAVE(x) taskData->saveVec.push(x)

#if (defined(_WIN32))
#include <tchar.h>
#define ERRORNUMBER _doserrno
#define NOMEMORY ERROR_NOT_ENOUGH_MEMORY
#else
typedef char TCHAR;
#define _T(x) x
#define _tfopen fopen
#define _tcslen strlen
#define _fputtc fputc
#define _fputts fputs
#define ERRORNUMBER errno
#define NOMEMORY ENOMEM
#endif

#ifdef _MSC_VER
// Don't tell me about ISO C++ changes.
#pragma warning(disable:4996)
#endif

extern "C" {
    POLYEXTERNALSYMBOL POLYUNSIGNED PolySaveState(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolySaveStateWithModules(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyShowModuleDependencies(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadState(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyGetHierarchy(POLYUNSIGNED threadId);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyRenameParent(POLYUNSIGNED threadId, POLYUNSIGNED childName, POLYUNSIGNED parentName);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyShowParent(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadHierarchy(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyStoreModule(POLYUNSIGNED threadId, POLYUNSIGNED filename, POLYUNSIGNED modName, POLYUNSIGNED contents);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadModule(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyGetModuleDirectory(POLYUNSIGNED threadId);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyShowLoadedModules(POLYUNSIGNED threadId);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyGetModuleInfo(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyReleaseModule(POLYUNSIGNED threadId, POLYUNSIGNED arg);
}

// This is probably generally useful so may be moved into
// a general header file.
template<typename BASE> class AutoFree
{
public:
    AutoFree(BASE p = 0) : m_value(p) {}
    ~AutoFree() { free(m_value); }

    // Automatic conversions to the base type.
    operator BASE() { return m_value; }
    BASE operator = (BASE p) { return (m_value = p); }

private:
    BASE m_value;
};

#ifdef HAVE__FTELLI64
// fseek and ftell are only 32-bits in Windows.
#define off_t   __int64
#define fseek _fseeki64
#define ftell _ftelli64
#endif

/*
 *  Structure definitions for the saved state files.
 */

#define SAVEDSTATESIGNATURE "POLYSAVE"
#define SAVEDSTATEVERSION   4
// Version 5 adds a module table: the modules the state depends on.  It is written
// only when the table is non-empty so other states keep the version 4 layout.
#define SAVEDSTATEVERSIONWITHMODULES   5
// Version 6 has the version 5 layout, but a segment that contains no code has no relocation
// table: if a segment it points into isn't where it was when saved, the loader finds the
// addresses by scanning its objects.  Written where saved states are mapped.
#define SAVEDSTATEVERSIONSCANNED   6

// File header for a saved state file.  This appears as the first entry
// in the file.
typedef struct _savedStateHeaderV4
{
    // These entries are primarily to check that we have a valid
    // saved state file before we try to interpret anything else.
    char        headerSignature[8];     // Should contain SAVEDSTATESIGNATURE
    unsigned    headerVersion;          // Should contain SAVEDSTATEVERSION
    unsigned    headerLength;           // Number of bytes in the header
    unsigned    segmentDescrLength;     // Number of bytes in a descriptor

    // These entries contain the real data.
    off_t       segmentDescr;           // Position of segment descriptor table
    unsigned    segmentDescrCount;      // Number of segment descriptors in the table
    off_t       stringTable;            // Pointer to the string table (zero if none)
    size_t      stringTableSize;        // Size of string table
    unsigned    parentNameEntry;        // Position of parent name in string table (0 if top)
    struct _moduleId      timeStamp;            // The time stamp for this file.
    struct _moduleId      parentTimeStamp;      // The time stamp for the parent.
    void       *originalBaseAddr;        // Original base address (32-in-64 only)
} SavedStateHeaderV4;

// Version 5 header: the version 4 fields followed by the module table.
typedef struct _savedStateHeaderV5
{
    SavedStateHeaderV4 v4;
    off_t       moduleTable;            // Position of the module table
    unsigned    moduleTableCount;       // Number of entries in the module table
} SavedStateHeaderV5;

// An entry in the module table is this followed by pathLength bytes of the path.
typedef struct _moduleTableEntry
{
    struct _moduleId    modId;          // The id the module had when the state was saved
    unsigned            pathLength;     // Length of the path in bytes, without a terminator
} ModuleTableEntry;

// The header as read.  For a version 4 file the module table fields are zero.
typedef SavedStateHeaderV5 SavedStateHeader;

// Entry for segment table.  This describes the segments on the disc that
// need to be loaded into memory.
typedef struct _savedStateSegmentDescr
{
    off_t       segmentData;            // Position of the segment data
    size_t      segmentSize;            // Size of the segment data
    off_t       relocations;            // Position of the relocation table
    unsigned    relocationCount;        // Number of entries in relocation table
    unsigned    relocationSize;         // Size of a relocation entry
    unsigned    segmentFlags;           // Segment flags (see SSF_ values)
    unsigned    segmentIndex;           // The index of this segment or the segment it overwrites
    struct _moduleId      moduleId;               // The module this came from.
    void       *originalAddr;           // Address of the segment when the file was saved
} SavedStateSegmentDescr;

#define SSF_WRITABLE    1               // The segment contains mutable data
#define SSF_OVERWRITE   2               // The segment overwrites the data (mutable) in a parent.
#define SSF_NOOVERWRITE 4               // The segment must not be further overwritten
#define SSF_BYTES       8               // The segment contains only byte data
#define SSF_CODE        16              // The segment contains only code

typedef struct _relocationEntry
{
    // Each entry indicates a location that has to be set to an address.
    // The location to be set is determined by adding "relocAddress" to the base address of
    // this segment (the one to which these relocations apply) and the value to store
    // by adding "targetAddress" to the base address of the segment indicated by targetIndex/targetModId.
    POLYUNSIGNED    relocAddress;       // The (byte) offset in this segment that we will set
    POLYUNSIGNED    targetAddress;      // The value to add to the base of the destination segment
    unsigned        targetSegment;      // Index into the address table
    ScanRelocationKind relKind;         // The kind of relocation (processor dependent).
} RelocationEntry;

// Read and check a saved state header.  Returns an error message or NULL.
static const char* readStateHeader(FILE* f, SavedStateHeader* header)
{
    memset(header, 0, sizeof(SavedStateHeader));
    if (fread(&header->v4, sizeof(SavedStateHeaderV4), 1, f) != 1)
        return "Unable to load header";
    if (strncmp(header->v4.headerSignature, SAVEDSTATESIGNATURE, sizeof(header->v4.headerSignature)) != 0)
        return "File is not a saved state";
    bool isV4 = header->v4.headerVersion == SAVEDSTATEVERSION &&
        header->v4.headerLength == sizeof(SavedStateHeaderV4);
    bool isV5 = (header->v4.headerVersion == SAVEDSTATEVERSIONWITHMODULES ||
            header->v4.headerVersion == SAVEDSTATEVERSIONSCANNED) &&
        header->v4.headerLength == sizeof(SavedStateHeaderV5);
    if (!(isV4 || isV5) || header->v4.segmentDescrLength != sizeof(SavedStateSegmentDescr))
        return "Unsupported version of saved state file";
    if (isV5 &&
        fread((char*)header + sizeof(SavedStateHeaderV4), sizeof(SavedStateHeaderV5) - sizeof(SavedStateHeaderV4), 1, f) != 1)
        return "Unable to load header";
    return NULL;
}

// A module table entry as read.
class ModuleTableItem {
public:
    ModuleId modId;
    std_tstring path;
};

// Read the module table of a saved state.  Returns an error message or NULL.
static const char* readModuleTable(FILE* f, const SavedStateHeader* header, std::vector<ModuleTableItem>& table)
{
    if (header->moduleTableCount == 0)
        return NULL;
    if (fseek(f, header->moduleTable, SEEK_SET) != 0)
        return "Unable to read module table";
    table.reserve(header->moduleTableCount);
    for (unsigned i = 0; i < header->moduleTableCount; i++)
    {
        ModuleTableEntry entry;
        if (fread(&entry, sizeof(entry), 1, f) != 1)
            return "Unable to read module table";
        std::string path(entry.pathLength, '\0');
        if (entry.pathLength != 0 && fread(&path[0], 1, entry.pathLength, f) != entry.pathLength)
            return "Unable to read module table";
        ModuleTableItem item;
        item.modId = ModuleId(entry.modId);
        item.path = std_tstring((TCHAR*)path.data(), entry.pathLength / sizeof(TCHAR));
        table.push_back(item);
    }
    return NULL;
}

#define SAVE(x) taskData->saveVec.push(x)

static void LogPhase(const char* phase, double wall, double cpu)
{
    if (debugOptions & DEBUG_SAVETIME)
        Log("SAVETIME: %s wall=%.3f cpu=%.3f\n", phase, wall, cpu);
}

// The wall and processor time of the phases of saving and loading, logged with --debug savetime.
class PhaseClock
{
public:
    PhaseClock() { Restart(); }
    void Restart() { wallStart = std::chrono::steady_clock::now(); cpuStart = std::clock(); }
    double Wall() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count(); }
    double Cpu() const { return (double)(std::clock() - cpuStart) / CLOCKS_PER_SEC; }
    // Log the time since the last restart as a phase, and restart.
    void Lap(const char* phase) { LogPhase(phase, Wall(), Cpu()); Restart(); }
    // Add the time since the last restart to totals, and restart.
    void AddTo(double& wall, double& cpu) { wall += Wall(); cpu += Cpu(); Restart(); }
private:
    std::chrono::steady_clock::time_point wallStart;
    std::clock_t cpuStart;
};

/*
 *  Hierarchy table: contains information about last loaded or saved state.
 */

// Pointer to list of files loaded in last load.
// There's no need for a lock since the update is only made when all
// the ML threads have stopped.
class HierarchyTable
{
public:
    HierarchyTable(const TCHAR *file, struct _moduleId time):
      fileName(_tcsdup(file)), timeStamp(time) { }

    HierarchyTable() {} // Needed for vector?

    std_tstring         fileName;
    struct _moduleId    timeStamp;
    std::vector<ModuleId> modules; // The modules in this level's module table.
};

std::vector <HierarchyTable> hierarchyTable;

// Information about currently loaded modules.  There will also
// be permanent memory spaces with the same Ids in gMem.

class LoadedModuleData {
public:
    ModuleId modId;
    std::vector<ModuleId> modDependencies;
    // The file the module was loaded from.  Empty if this process stored it.
    std_tstring fileName;
};

static std::vector<LoadedModuleData> loadedModules;
// The ids in loadedModules, to check quickly whether a module is loaded.
static std::unordered_set<uint64_t> loadedModuleIds;

static uint64_t ModuleKey(ModuleId m)
{
    struct _moduleId id = m;
    return (uint64_t)id.modA << 32 | id.modB;
}

class ModIdAndName {
public:
    ModIdAndName(std_tstring mName, ModuleId mId, bool isM): modName(mName), modId(mId), isModule(isM) {}
    std_tstring modName;
    ModuleId modId;
    bool isModule; // True if a module, false if a saved state.
};


// Test whether we're overwriting a parent of ourself.
#if (defined(_WIN32) || defined(__CYGWIN__))
static bool sameFile(const TCHAR *x, const TCHAR *y)
{
    HANDLE hXFile = INVALID_HANDLE_VALUE, hYFile = INVALID_HANDLE_VALUE;
    bool result = false;

    hXFile = CreateFile(x, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hXFile == INVALID_HANDLE_VALUE) goto closeAndExit;
    hYFile = CreateFile(y, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hYFile == INVALID_HANDLE_VALUE) goto closeAndExit;
    BY_HANDLE_FILE_INFORMATION fileInfoX, fileInfoY;
    if (! GetFileInformationByHandle(hXFile, &fileInfoX)) goto closeAndExit;
    if (! GetFileInformationByHandle(hYFile, &fileInfoY)) goto closeAndExit;

    result = fileInfoX.dwVolumeSerialNumber == fileInfoY.dwVolumeSerialNumber &&
        fileInfoX.nFileIndexLow == fileInfoY.nFileIndexLow &&
        fileInfoX.nFileIndexHigh == fileInfoY.nFileIndexHigh;

closeAndExit:
    if (hXFile != INVALID_HANDLE_VALUE) CloseHandle(hXFile);
    if (hYFile != INVALID_HANDLE_VALUE) CloseHandle(hYFile);
    return result;
}
#else
static bool sameFile(const char *x, const char *y)
{
    struct stat xStat, yStat;
    // If either file does not exist that's fine.
    if (stat(x, &xStat) != 0 || stat(y, &yStat) != 0)
        return false;
    return (xStat.st_dev == yStat.st_dev && xStat.st_ino == yStat.st_ino);
}
#endif

/*
 *  Saving state.
 */

// Whether the objects from start to end include a code object.
static bool containsCode(PolyWord* start, PolyWord* end)
{
    for (PolyWord* p = start; p < end; )
    {
        p++;
        PolyObject* obj = (PolyObject*)p;
        if (obj->IsCodeObject())
            return true;
        p += obj->Length();
    }
    return false;
}

 // Request to the main thread to save data.
class SaveRequest : public MainThreadRequest, public StateExport
{
public:
    SaveRequest(const TCHAR* name, unsigned h, bool withMods) : MainThreadRequest(MTP_SAVESTATE),
        fileName(name), newHierarchy(h), withModules(withMods), relocationCount(0) {
    }

    virtual void Perform();
    const TCHAR* fileName;
    unsigned newHierarchy;
    // If true, modules loaded from files are dependencies rather than copied.
    bool withModules;

public:
    virtual void exportStore(void) {} // Not used.

protected:
    void createActualRelocation(void* addr, void* relocAddr, ScanRelocationKind kind);
    unsigned relocationCount;
};

// Create a relocation entry for an address at a given location.
// This is currently never called in compact 32-bit mode because Exporter::relocateValue does
// not call createRelocation in 32-in-64 mode.
void SaveRequest::createActualRelocation(void *addr, void* relocAddr, ScanRelocationKind kind)
{
    RelocationEntry reloc;
    // Set the offset within the section we're scanning.
    setRelocationAddress(relocAddr, &reloc.relocAddress);

    unsigned addrArea = findArea(addr);
    reloc.targetAddress = (POLYUNSIGNED)((char*)addr - (char*)memTable[addrArea].mtOriginalAddr);
    reloc.targetSegment = addrArea;
    reloc.relKind = kind;
    fwrite(&reloc, sizeof(reloc), 1, exportFile);
    relocationCount++;
}

// This class is used to update references to objects that have moved.  If
// we have copied an object into the area to be exported we may still have references
// to it from the stack or from RTS data structures.  We have to ensure that these
// are updated.
// This is very similar to ProcessFixupAddress in sharedata.cpp
class SaveFixupAddress: public ScanAddress
{
protected:
    virtual POLYUNSIGNED ScanAddressAt(PolyWord *pt);
    virtual POLYUNSIGNED ScanCodeAddressAt(PolyObject **pt) { *pt = ScanObjectAddress(*pt); return 0; }
    virtual PolyObject *ScanObjectAddress(PolyObject *base);

public:
    void ScanCodeSpace(CodeSpace *space);
};


POLYUNSIGNED SaveFixupAddress::ScanAddressAt(PolyWord *pt)
{
    PolyWord val = *pt;
    if (val.IsDataPtr() && val != PolyWord::FromUnsigned(0))
        *pt = ScanObjectAddress(val.AsObjPtr());
    return 0;
}

// Returns the new address if the argument is the address of an object that
// has moved, otherwise returns the original.
PolyObject *SaveFixupAddress::ScanObjectAddress(PolyObject *obj)
{
    if (obj->ContainsForwardingPtr()) // tombstone is a pointer to a moved object
    {
#ifdef POLYML32IN64
        MemSpace *space = gMem.SpaceForAddress((PolyWord*)obj - 1);
        PolyObject *newp;
        if (space->isCode)
            newp = (PolyObject*)(globalCodeBase + ((obj->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
        else newp = obj->GetForwardingPtr();
#else
        PolyObject *newp = obj->GetForwardingPtr();
#endif
        ASSERT (newp->ContainsNormalLengthWord());
        return newp;
    }
    
    ASSERT (obj->ContainsNormalLengthWord()); // object is not moved
    return obj;
}

// Fix up addresses in the code area.  Unlike ScanAddressesInRegion this updates
// cells that have been moved.  We need to do that because we may still have
// return addresses into those cells and we don't move return addresses.  We
// do want the code to see updated constant addresses.
void SaveFixupAddress::ScanCodeSpace(CodeSpace *space)
{
    for (PolyWord *pt = space->bottom; pt < space->top; )
    {
        pt++;
        PolyObject *obj = (PolyObject*)pt;
#ifdef POLYML32IN64
        PolyObject *dest = obj;
        while (dest->ContainsForwardingPtr())
        {
            MemSpace *space = gMem.SpaceForObjectAddress(dest);
            if (space->isCode)
                dest = (PolyObject*)(globalCodeBase + ((dest->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
            else dest = dest->GetForwardingPtr();
        }
#else
        PolyObject *dest = obj->FollowForwardingChain();
#endif
        POLYUNSIGNED length = dest->Length();
        if (length != 0)
            ScanAddressesInObject(obj, dest->LengthWord());
        pt += length;
    }
}

// Exported function also used by the modules system.
void switchLocalsToPermanent()
{
    // Update references to moved objects.
    SaveFixupAddress fixup;
    for (std::vector<LocalMemSpace*>::iterator i = gMem.lSpaces.begin(); i < gMem.lSpaces.end(); i++)
    {
        LocalMemSpace* space = *i;
        fixup.ScanAddressesInRegion(space->bottom, space->lowerAllocPtr);
        fixup.ScanAddressesInRegion(space->upperAllocPtr, space->top);
    }
    for (std::vector<CodeSpace*>::iterator i = gMem.cSpaces.begin(); i < gMem.cSpaces.end(); i++)
        fixup.ScanCodeSpace(*i);

    GCModules(&fixup);

    // Restore the length words in the code areas.
    // Although we've updated any pointers to the start of the code we could have return addresses
    // pointing to the original code.  GCModules updates the stack but doesn't update return addresses.
    for (std::vector<CodeSpace*>::iterator i = gMem.cSpaces.begin(); i < gMem.cSpaces.end(); i++)
    {
        CodeSpace* space = *i;
        for (PolyWord* pt = space->bottom; pt < space->top; )
        {
            pt++;
            PolyObject* obj = (PolyObject*)pt;
            if (obj->ContainsForwardingPtr())
            {
#ifdef POLYML32IN64
                PolyObject* forwardedTo = obj;
                while (forwardedTo->ContainsForwardingPtr())
                    forwardedTo = (PolyObject*)(globalCodeBase + ((forwardedTo->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
#else
                PolyObject* forwardedTo = obj->FollowForwardingChain();
#endif
                POLYUNSIGNED lengthWord = forwardedTo->LengthWord();
                space->writeAble(obj)->SetLengthWord(lengthWord);
            }
            pt += obj->Length();
        }
    }
}

// Called by the root thread to actually save the state and write the file.
void SaveRequest::Perform()
{
    try {
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Beginning saving state.\n");
        // Check that we aren't overwriting our own parent.
        for (unsigned q = 0; q < newHierarchy - 1; q++) {
            if (sameFile(hierarchyTable[q].fileName.c_str(), fileName))
            {
                errorMessage = "File being saved is used as a parent of this file";
                errNumber = 0;
                if (debugOptions & DEBUG_SAVING)
                    Log("SAVE: File being saved is used as a parent of this file.\n");
                return;
            }
        }
        // Nor a loaded module's file, which the state may depend on.
        for (std::vector<LoadedModuleData>::const_iterator m = loadedModules.cbegin(); m != loadedModules.cend(); m++)
        {
            if (!m->fileName.empty() && sameFile(m->fileName.c_str(), fileName))
            {
                errorMessage = "File being saved is a loaded module";
                errNumber = 0;
                return;
            }
        }

#ifdef POLYML_MAP_SAVED_STATES
        // Other processes may have the existing file mapped: write a new file rather
        // than truncating theirs, which would raise SIGBUS in them.  As truncating did,
        // write to the file a symbolic link names and keep the file's permissions.
        char *existing = realpath(fileName, NULL); // NULL if there's no file yet.
        std::string target(existing != NULL ? existing : fileName);
        free(existing);
        struct stat oldStat;
        bool replacing = stat(target.c_str(), &oldStat) == 0;
        mode_t mode = replacing ? (oldStat.st_mode & 0777) : 0666;
        if (replacing && unlink(target.c_str()) != 0 && errno != ENOENT)
        {
            errorMessage = "Cannot replace save file";
            errNumber = ERRORNUMBER;
            if (debugOptions & DEBUG_SAVING)
                Log("SAVE: Cannot replace save file.\n");
            return;
        }
        // Created with at most the old permissions, so no one else can open it meanwhile.
        // A replacement must be a new file: if another save created one since the unlink,
        // fail rather than truncate that.
        int fd = open(target.c_str(), O_WRONLY | O_CREAT | (replacing ? O_EXCL : O_TRUNC), mode);
        if (fd >= 0 && ((replacing && fchmod(fd, mode) != 0) || (exportFile = fdopen(fd, "wb")) == NULL))
        {
            int err = errno;
            close(fd);
            errno = err;
        }
#else
        // Open the file.  This could quite reasonably fail if the path is wrong.
        exportFile = _tfopen(fileName, _T("wb"));
#endif
        if (exportFile == NULL)
        {
            errorMessage = "Cannot open save file";
            errNumber = ERRORNUMBER;
            if (debugOptions & DEBUG_SAVING)
                Log("SAVE: Cannot open save file.\n");
            return;
        }

        // Scan over the permanent mutable area copying all reachable data that is
        // not in a lower hierarchy into new permanent segments.
        CopyScan copyScan;
        // Set the dependencies.
        copyScan.dependencies[exportSignature] = true; // Always include the executable
        // Include any existing states at a higher level
        for (unsigned i = 0; i < newHierarchy - 1; i++)
            copyScan.dependencies[hierarchyTable[i].timeStamp] = true;
        // The modules a retained level recorded are loaded with it, so they are dependencies
        // whether or not this state records modules itself.
        std::unordered_set<uint64_t> recorded;
        for (unsigned i = 0; i < newHierarchy - 1; i++)
            for (std::vector<ModuleId>::const_iterator m = hierarchyTable[i].modules.cbegin(); m != hierarchyTable[i].modules.cend(); m++)
                if (loadedModuleIds.count(ModuleKey(*m)) != 0)
                {
                    recorded.insert(ModuleKey(*m));
                    copyScan.dependencies[*m] = true;
                }
        // The modules this state depends on that no retained level recorded.
        std::vector<const LoadedModuleData*> moduleTable;
        if (withModules)
        {
            // A module is a dependency if it was loaded from a file and everything it depends on
            // is retained.  Modules are loaded after their dependencies so one pass in load order is enough.
            for (std::vector<LoadedModuleData>::const_iterator m = loadedModules.cbegin(); m != loadedModules.cend(); m++)
            {
                if (m->fileName.empty()) continue;
                bool depsRetained = true;
                for (std::vector<ModuleId>::const_iterator d = m->modDependencies.cbegin(); d != m->modDependencies.cend(); d++)
                {
                    std::map<ModuleId, bool>::const_iterator f = copyScan.dependencies.find(*d);
                    if (f == copyScan.dependencies.end() || !f->second) { depsRetained = false; break; }
                }
                if (!depsRetained) continue;
                copyScan.dependencies[m->modId] = true;
                if (recorded.count(ModuleKey(m->modId)) == 0)
                    moduleTable.push_back(&*m);
            }
        }
        bool success = true;
        PhaseClock clock;
#ifdef POLYML_PLACE_SAVED_CODE
        gMem.SetSavedCodeDepth(newHierarchy);
#endif
        try {
            copyScan.initialise();
            // By index: making an export space reserves pSpaces capacity, which may move it.
            for (size_t n = 0; n < gMem.pSpaces.size(); n++)
            {
                PermanentMemSpace* space = gMem.pSpaces[n];
                if (space->isMutable && !space->noOverwrite && !space->byteOnly)
                {
                    if (debugOptions & DEBUG_SAVING)
                        Log("SAVE: Scanning permanent mutable area %p allocated at %p size %lu\n",
                            space, space->bottom, space->spaceSize());
                    copyScan.ScanAddressesInRegion(space->bottom, space->top);
                }
            }
            clock.Lap("save-scan");
            // We may have copied data out of modules.  Restore forwarding pointers in them.
            CopyScan::fixPermanentAreas();
            clock.Lap("save-fixperm");
        }
        catch (MemoryException&)
        {
            success = false;
            if (debugOptions & DEBUG_SAVING)
                Log("SAVE: Scan of permanent mutable area raised memory exception.\n");
        }
#ifdef POLYML_PLACE_SAVED_CODE
        gMem.SetSavedCodeDepth(0);
#endif

        // Copy the areas into the export object.  Make sufficient space for
        // the largest possible number of entries.
        memTable = new ExportMemTable[gMem.eSpaces.size() + gMem.pSpaces.size() + 1];
        unsigned memTableCount = 0;

        // Permanent spaces at higher level.  These have to have entries although
        // only the mutable entries will be written.
        for (std::vector<PermanentMemSpace*>::iterator i = gMem.pSpaces.begin(); i < gMem.pSpaces.end(); i++)
        {
            PermanentMemSpace* space = *i;
            if (copyScan.dependencies[space->moduleIdentifier])
            {
                ExportMemTable* entry = &memTable[memTableCount++];
                entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
                entry->mtLength = (space->topPointer - space->bottom) * sizeof(PolyWord);
                entry->mtIndex = space->index;
                entry->mtFlags = 0;
                entry->mtModId = space->moduleIdentifier;
                if (space->isMutable)
                {
                    entry->mtFlags |= MTF_WRITEABLE;
                    if (space->noOverwrite) entry->mtFlags |= MTF_NO_OVERWRITE;
                    if (space->byteOnly) entry->mtFlags |= MTF_BYTES;
                }
                if (space->isCode)
                    entry->mtFlags |= MTF_EXECUTABLE;
            }
        }
        unsigned permanentEntries = memTableCount; // Remember where new entries start.

        exportModId = copyScan.extractHash();

        // Newly created spaces.
        for (std::vector<PermanentMemSpace*>::iterator i = gMem.eSpaces.begin(); i < gMem.eSpaces.end(); i++)
        {
            ExportMemTable* entry = &memTable[memTableCount++];
            PermanentMemSpace* space = *i;
            entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
            entry->mtLength = (space->topPointer - space->bottom) * sizeof(PolyWord);
            entry->mtIndex = space->index;
            entry->mtFlags = 0;
            entry->mtModId = exportModId;
            if (space->isMutable)
            {
                entry->mtFlags |= MTF_WRITEABLE;
                if (space->noOverwrite) entry->mtFlags |= MTF_NO_OVERWRITE;
                if (space->byteOnly) entry->mtFlags |= MTF_BYTES;
            }
            if (space->isCode)
                entry->mtFlags |= MTF_EXECUTABLE;
        }

        memTableEntries = memTableCount;

        // Promoting the export spaces after the locals are switched can't fail: the
        // pSpaces capacity it needs was reserved as each export space was made.
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Updating references to moved objects.\n");
        // We always switch to the permanent space even if there's been a
        // failure since it is possible to re-save.
        switchLocalsToPermanent();

        // Update the global memory space table.  Old segments at the same level
        // or lower are removed.  The new segments become permanent.
        // Try to promote the spaces even if we've had a failure because export
        // spaces are deleted in ~CopyScan and we may have already copied
        // some objects there.
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Promoting export spaces to permanent spaces.\n");
        // Remove any deeper entries from the hierarchy table.
        for (unsigned i = newHierarchy - 1; i < hierarchyTable.size(); i++)
        {
            if (!gMem.DemoteOldPermanentSpaces(hierarchyTable[i].timeStamp))
            {
                errorMessage = "Out of Memory";
                errNumber = NOMEMORY;
                if (debugOptions & DEBUG_SAVING)
                    Log("SAVE: Unable to demote old spaces.\n");
            }
        }
        hierarchyTable.resize(newHierarchy - 1);

        if (!gMem.PromoteNewExportSpaces(exportModId) || !success)
        {
            errorMessage = "Out of Memory";
            errNumber = NOMEMORY;
            if (debugOptions & DEBUG_SAVING)
                Log("SAVE: Unable to promote export spaces.\n");
            return;
        }

        clock.Lap("save-promote");
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Writing out data.\n");

        // TODO: We should handle write failures e.g. if the disc becomes full.

        // Write out the file header.  The version 5 layout only if there is a module table
        // or the segments without code have no relocations.
#ifdef POLYML_MAP_SAVED_STATES
        const bool scanned = true;
#else
        const bool scanned = false;
#endif
        SavedStateHeaderV5 fullHeader;
        memset(&fullHeader, 0, sizeof(fullHeader));
        SavedStateHeaderV4& saveHeader = fullHeader.v4;
        size_t headerSize = moduleTable.empty() && !scanned ? sizeof(SavedStateHeaderV4) : sizeof(SavedStateHeaderV5);
        saveHeader.headerLength = (unsigned)headerSize;
        memcpy(saveHeader.headerSignature,
            SAVEDSTATESIGNATURE, sizeof(saveHeader.headerSignature));
        saveHeader.headerVersion = scanned ? SAVEDSTATEVERSIONSCANNED :
            moduleTable.empty() ? SAVEDSTATEVERSION : SAVEDSTATEVERSIONWITHMODULES;
        saveHeader.segmentDescrLength = sizeof(SavedStateSegmentDescr);
        if (newHierarchy == 1)
            saveHeader.parentTimeStamp = exportSignature;
        else
        {
            saveHeader.parentTimeStamp = hierarchyTable[newHierarchy - 2].timeStamp;
            saveHeader.parentNameEntry = sizeof(TCHAR); // Always the first entry.
        }
        saveHeader.timeStamp = copyScan.extractHash();
        saveHeader.segmentDescrCount = memTableEntries; // One segment for each space.
#ifdef POLYML32IN64
        saveHeader.originalBaseAddr = globalHeapBase;
#endif
        // Write out the header.
        fwrite(&fullHeader, headerSize, 1, exportFile);

        // We need a segment header for each permanent area whether it is
        // actually in this file or not.
        std::vector<SavedStateSegmentDescr> descrs;
        descrs.reserve(memTableEntries);
        // Write out temporarily. Will be overwritten at the end.
        saveHeader.segmentDescr = ftell(exportFile);

        for (unsigned j = 0; j < memTableEntries; j++)
        {
            ExportMemTable* entry = &memTable[j];
            SavedStateSegmentDescr descr;
            memset(&descr, 0, sizeof(SavedStateSegmentDescr));
            descr.relocationSize = sizeof(RelocationEntry);
            descr.segmentIndex = (unsigned)entry->mtIndex;
            descr.segmentSize = entry->mtLength; // Set this even if we don't write it.
            descr.moduleId = entry->mtModId;
            descr.originalAddr = entry->mtOriginalAddr;
            if (entry->mtFlags & MTF_WRITEABLE)
            {
                descr.segmentFlags |= SSF_WRITABLE;
                if (entry->mtFlags & MTF_NO_OVERWRITE)
                    descr.segmentFlags |= SSF_NOOVERWRITE;
                if (j < permanentEntries && (entry->mtFlags & MTF_NO_OVERWRITE) == 0)
                    descr.segmentFlags |= SSF_OVERWRITE;
                if (entry->mtFlags & MTF_BYTES)
                    descr.segmentFlags |= SSF_BYTES;
            }
            if (entry->mtFlags & MTF_EXECUTABLE)
                descr.segmentFlags |= SSF_CODE;
            if(!checkedFwrite(&descr, sizeof(SavedStateSegmentDescr), 1))
                return;
            descrs.push_back(descr);
        }

        // Write out the relocations and the data.
        double relocWall = 0, relocCpu = 0, dataWall = 0, dataCpu = 0;
        uint64_t relocTotal = 0;
        for (unsigned k = 1 /* Not IO area */; k < memTableEntries; k++)
        {
            ExportMemTable* entry = &memTable[k];
            // Write out the contents if this is new or if it is a normal, overwritable
            // mutable area.
            if (k >= permanentEntries ||
                (entry->mtFlags & (MTF_WRITEABLE | MTF_NO_OVERWRITE)) == MTF_WRITEABLE)
            {
                char* start = (char*)entry->mtOriginalAddr;
                char* end = start + entry->mtLength;
                // Have to write this out.
                relocationCount = 0;
                // Create the relocation table, except where the loader scans the objects.
                descrs[k].relocations = 0;
                if (!scanned || (entry->mtFlags & MTF_EXECUTABLE) || containsCode((PolyWord*)start, (PolyWord*)end))
                {
                    descrs[k].relocations = ftell(exportFile);
                    for (PolyWord* p = (PolyWord*)start; p < (PolyWord*)end; )
                    {
                        p++;
                        PolyObject* obj = (PolyObject*)p;
                        POLYUNSIGNED length = obj->Length();
                        // Now include all relocations rather than trying to sort them out when loading.
                        if (length != 0 && obj->IsCodeObject())
                            machineDependent->ScanConstantsWithinCode(obj, this);
                        relocateObject(obj);
                        p += length;
                    }
                }
                descrs[k].relocationCount = relocationCount;
                relocTotal += relocationCount;
                clock.AddTo(relocWall, relocCpu);
                // Write out the data, starting at a multiple of 64K so that it can be mapped.
                static const char zeros[4096] = { 0 };
                for (off_t pos = ftell(exportFile); pos % (64 * 1024) != 0; )
                {
                    size_t pad = (size_t)(64 * 1024 - pos % (64 * 1024));
                    if (pad > sizeof(zeros)) pad = sizeof(zeros);
                    if (!checkedFwrite(zeros, pad, 1))
                        return;
                    pos += pad;
                }
                descrs[k].segmentData = ftell(exportFile);
                if (!checkedFwrite(entry->mtOriginalAddr, entry->mtLength, 1))
                    return;
                clock.AddTo(dataWall, dataCpu);
            }
        }
        LogPhase("save-relocs", relocWall, relocCpu);
        LogPhase("save-data", dataWall, dataCpu);
        if (debugOptions & DEBUG_SAVETIME)
            Log("SAVETIME: save-relocations count=%llu bytes=%llu\n",
                (unsigned long long)relocTotal, (unsigned long long)(relocTotal * sizeof(RelocationEntry)));

        // If this is a child we need to write a string table containing the parent name.
        if (newHierarchy > 1)
        {
            saveHeader.stringTable = ftell(exportFile);
            _fputtc(0, exportFile); // First byte of string table is zero
            _fputts(hierarchyTable[newHierarchy - 2].fileName.c_str(), exportFile);
            _fputtc(0, exportFile); // A terminating null.
            saveHeader.stringTableSize = (_tcslen(hierarchyTable[newHierarchy - 2].fileName.c_str()) + 2) * sizeof(TCHAR);
        }

        if (ferror(exportFile))
        {
            errNumber = ERRORNUMBER;
            errorMessage = "Error while writing string table";
            return;
        }

        // The module table, if any.
        if (!moduleTable.empty())
        {
            fullHeader.moduleTable = ftell(exportFile);
            fullHeader.moduleTableCount = (unsigned)moduleTable.size();
            for (std::vector<const LoadedModuleData*>::const_iterator m = moduleTable.cbegin(); m != moduleTable.cend(); m++)
            {
                ModuleTableEntry entry;
                memset(&entry, 0, sizeof(entry));
                entry.modId = (*m)->modId;
                entry.pathLength = (unsigned)((*m)->fileName.size() * sizeof(TCHAR));
                if (!checkedFwrite(&entry, sizeof(entry), 1) ||
                    !checkedFwrite((*m)->fileName.data(), 1, entry.pathLength))
                    return;
            }
        }

        // Rewrite the header and the segment tables now they're complete.
        fseek(exportFile, 0, SEEK_SET);
        if (!checkedFwrite(&fullHeader, headerSize, 1))
            return;
        if (!checkedFwrite(descrs.data(), sizeof(SavedStateSegmentDescr), memTableEntries))
            return;

        clock.Lap("save-tables");
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Writing complete.\n");

        // Add an entry to the hierarchy table for this file.
        HierarchyTable level(fileName, saveHeader.timeStamp);
        for (std::vector<const LoadedModuleData*>::const_iterator m = moduleTable.cbegin(); m != moduleTable.cend(); m++)
            level.modules.push_back((*m)->modId);
        hierarchyTable.push_back(level);

        CheckMemory();
    }

    catch (const std::bad_alloc&)
    {
        errorMessage = "Insufficient Memory: C++ allocation failed";
    }
}

// Write a saved state file.
static POLYUNSIGNED saveState(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth, bool withModules)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();

    try {
        TempString fileNameBuff(Poly_string_to_T_alloc(PolyWord::FromUnsigned(fileName)));
        // The value of depth is zero for top-level save so we need to add one for hierarchy.
        unsigned newHierarchy = get_C_unsigned(taskData, PolyWord::FromUnsigned(depth)) + 1;

        if (newHierarchy > hierarchyTable.size() + 1)
            raise_fail(taskData, "Depth must be no more than the current hierarchy plus one");

        // Request a full GC first.  The main reason is to avoid running out of memory as a
        // result of repeated saves.  Old export spaces are turned into local spaces and
        // the GC will delete them if they are completely empty
        PhaseClock clock;
        FullGC(taskData);
        clock.Lap("save-fullgc");

        SaveRequest request(fileNameBuff, newHierarchy, withModules);
        processes->MakeRootRequest(taskData, &request);
        clock.Lap("save-request");
        if (request.errorMessage)
            raise_syscall(taskData, request.errorMessage, request.errNumber);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

POLYUNSIGNED PolySaveState(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth)
{
    return saveState(threadId, fileName, depth, false);
}

// Save a state that refers to the modules loaded from files instead of copying their data.
POLYUNSIGNED PolySaveStateWithModules(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth)
{
    return saveState(threadId, fileName, depth, true);
}

/*
 *  Loading saved state files.
 */

class StateLoader: public MainThreadRequest
{
public:
    StateLoader(bool isH, Handle files): MainThreadRequest(MTP_LOADSTATE),
        isHierarchy(isH), fileNameList(files), errorResult(0), errNumber(0) { }

    virtual void Perform(void);
    bool LoadFile(bool isInitial, ModuleId requiredStamp, PolyWord tail);
    bool isHierarchy;
    Handle fileNameList;
    const char *errorResult;
    std::string errorBuffer; // Holds errorResult when it is constructed.
    // The fileName here is the last file loaded.  As well as using it
    // to load the name can also be printed out at the end to identify the
    // particular file in the hierarchy that failed.
    AutoFree<TCHAR*> fileName;
    int errNumber;
};

// Data needed for relocation during load.
class StateLoadData {
public:
    StateLoadData() : relocations(0), relocationCount(0), targetAddr(0) {}

    off_t       relocations;        // Copied from descriptor
    unsigned    relocationCount;
    PolyWord* targetAddr;           // Actual address of the segment
};

// Called by the main thread once all the ML threads have stopped.
void StateLoader::Perform(void)
{
    try {
        // Copy the first file name into the buffer.
        if (isHierarchy)
        {
            if (ML_Cons_Cell::IsNull(fileNameList->Word()))
            {
                errorResult = "Hierarchy list is empty";
                return;
            }
            ML_Cons_Cell* p = DEREFLISTHANDLE(fileNameList);
            fileName = Poly_string_to_T_alloc(p->h);
            if (fileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return;
            }
            (void)LoadFile(true, ModuleId(), p->t);
        }
        else
        {
            fileName = Poly_string_to_T_alloc(fileNameList->Word());
            if (fileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return;
            }
            (void)LoadFile(true, ModuleId(), TAGGED(0));
        }
    }
    catch (const std::bad_alloc&)
    {
        errorResult = "Insufficient Memory: C++ allocation failed";
    }
}

// Set the values of external references and clear the values of other weak byte refs.
void ClearVolatile::ScanAddressesInObject(PolyObject *base, POLYUNSIGNED lengthWord)
{
    if (OBJ_IS_MUTABLE_OBJECT(lengthWord) && OBJ_IS_NO_OVERWRITE(lengthWord))
    {
        if (OBJ_IS_BYTE_OBJECT(lengthWord))
        {
            if (OBJ_IS_WEAKREF_OBJECT(lengthWord))
            {
                POLYUNSIGNED len = OBJ_OBJECT_LENGTH(lengthWord);
                if (len >= sizeof(uintptr_t) / sizeof(PolyWord))
                    *((uintptr_t*)base) = 0;
                setEntryPoint(base);
            }
        }
        else
        {
            // Clear volatile refs
            POLYUNSIGNED len = OBJ_OBJECT_LENGTH(lengthWord);
            for (POLYUNSIGNED i = 0; i < len; i++)
                base->Set(i, TAGGED(0));
        }
    }
}

// Work around bug in Mac OS when reading into MAP_JIT memory.
size_t readData(void *ptr, size_t size, FILE *stream)
{
#ifndef MACOSX
    return fread(ptr, size, 1, stream);
#else
    char buff[1024];
    for (size_t s = 0; s < size; )
    {
        size_t unit = sizeof(buff);
        if (size - s < unit) unit = size-s;
        if (fread(buff, unit, 1, stream) != 1)
            return 0;
        memcpy((char*)ptr+s, buff, unit);
        s += unit;
    }
    return 1; // Succeeded
#endif
}

static bool loadModuleFile(const std_tstring& fileName, const ModuleId* requiredId,
    const char*& errorResult, int& errNumber, PolyObject*& root, ModuleId& loadedId);

// The number of saved state and module files loaded so far that had segments not where
// they were saved, so that relocations were read.
static unsigned movedStateLoads = 0, movedModuleLoads = 0;

// Relocate a segment saved without a relocation table: add to each address in its objects
// how far the segment it points into has moved.  byAddress lists the segments, by index,
// in the order of their original addresses.  Returns an error message or 0.
static const char *relocateByScanning(PolyWord *base, size_t segmentSize, const std::vector<SavedStateSegmentDescr> &descrs,
    const std::vector<std::pair<uintptr_t, size_t> > &byAddress,
    const std::vector<StateLoadData> &loadData, const std::vector<bool> &moved, uint64_t &relocsApplied)
{
    PolyWord *end = base + segmentSize / sizeof(PolyWord);
    for (PolyWord *p = base; p < end; )
    {
        p++;
        PolyObject *obj = (PolyObject*)p;
        POLYUNSIGNED length = obj->Length();
        if (length > (POLYUNSIGNED)(end - p))
            return "Object in saved state runs past its segment";
        if (obj->IsCodeObject())
            return "Code in a saved state segment without relocations";
        if (!obj->IsByteObject())
        {
            for (POLYUNSIGNED i = 0; i < length; i++)
            {
                PolyWord w = p[i];
                if (w.IsTagged() || w == PolyWord::FromUnsigned(0))
                    continue;
                // The last segment that starts below the address.  An address can be the end
                // of a segment if a zero-length object is last in it.
                uintptr_t a = w.AsUnsigned();
                std::vector<std::pair<uintptr_t, size_t> >::const_iterator s =
                    std::lower_bound(byAddress.begin(), byAddress.end(), std::make_pair(a, (size_t)0));
                if (s == byAddress.begin())
                    return "Address in saved state is outside its segments";
                s--;
                size_t t = s->second;
                if (a > s->first + descrs[t].segmentSize)
                    return "Address in saved state is outside its segments";
                if (moved[t])
                {
                    p[i] = PolyWord::FromUnsigned(a - s->first + (uintptr_t)loadData[t].targetAddr);
                    relocsApplied++;
                }
            }
        }
        p += length;
    }
    return 0;
}

// Load a saved state file.  Calls itself to handle parent files.
bool StateLoader::LoadFile(bool isInitial, ModuleId requiredStamp, PolyWord tail)
{
    AutoFree<TCHAR*> thisFile(_tcsdup(fileName));

    AutoClose loadFile(_tfopen(fileName, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        errorResult = "Cannot open load file";
        errNumber = ERRORNUMBER;
        return false;
    }

    SavedStateHeader fullHeader;
    // Read the header and check the signature.
    errorResult = readStateHeader(loadFile, &fullHeader);
    if (errorResult != NULL)
        return false;
    SavedStateHeaderV4& header = fullHeader.v4;

    // Check that we have the required stamp before loading any children.
    // If a parent has been overwritten we could get a loop.
    if (! isInitial && ModuleId(header.timeStamp) != requiredStamp)
    {
        // Time-stamps don't match.
        errorResult = "The parent for this saved state does not match or has been changed";
        return false;
    }

    // Have verified that this is a reasonable saved state file.  If it isn't a
    // top-level file we have to load the parents first.
    if (header.parentNameEntry != 0)
    {
        if (isHierarchy)
        {
            // Take the file name from the list
            if (ML_Cons_Cell::IsNull(tail))
            {
                errorResult = "Missing parent name in argument list";
                return false;
            }
            ML_Cons_Cell *p = (ML_Cons_Cell *)tail.AsObjPtr();
            fileName = Poly_string_to_T_alloc(p->h);
            if (fileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return false;
            }
            if (! LoadFile(false, header.parentTimeStamp, p->t))
                return false;
        }
        else
        {
            size_t toRead = header.stringTableSize-header.parentNameEntry;
            size_t elems = ((toRead + sizeof(TCHAR) - 1) / sizeof(TCHAR));
            // Always allow space for null terminator
            size_t roundedBytes = (elems + 1) * sizeof(TCHAR);
            TCHAR *newFileName = (TCHAR *)realloc(fileName, roundedBytes);
            if (newFileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return false;
            }
            fileName = newFileName;

            if (header.parentNameEntry >= header.stringTableSize /* Bad entry */ ||
                fseek(loadFile, header.stringTable + header.parentNameEntry, SEEK_SET) != 0 ||
                fread(fileName, 1, toRead, loadFile) != toRead)
            {
                errorResult = "Unable to read parent file name";
                return false;
            }
            fileName[elems] = 0; // Should already be null-terminated, but just in case.

            if (! LoadFile(false, header.parentTimeStamp, TAGGED(0)))
                return false;
        }

        ASSERT(hierarchyTable.size() > 0);
    }
    else // Top-level file
    {
        if (isHierarchy && ! ML_Cons_Cell::IsNull(tail))
        {
            // There should be no further file names if this is really the top.
            errorResult = "Too many file names in the list";
            return false;
        }
        if (ModuleId(header.parentTimeStamp) != exportSignature)
        {
            // Time-stamp does not match executable.
            errorResult = 
                    "Saved state was exported from a different executable or the executable has changed";
            return false;
        }

        // Any existing spaces at this level or greater must be turned
        // into local spaces.  We may have references from the stack to objects that
        // have previously been imported but otherwise these spaces are no longer
        // needed.
        // Clean out the hierarchy table.
        for (std::vector<HierarchyTable>::iterator i = hierarchyTable.begin(); i != hierarchyTable.end(); i++)
            gMem.DemoteOldPermanentSpaces(i->timeStamp);

        hierarchyTable.clear();

        for (std::vector<LoadedModuleData>::iterator i = loadedModules.begin(); i != loadedModules.end(); i++)
            gMem.DemoteOldPermanentSpaces(i->modId);

        loadedModules.clear();
        loadedModuleIds.clear();

    }

    // Load the modules this state depends on that aren't already loaded.  This is done
    // after the parents are loaded because loading the top-level state unloads all modules,
    // and before this state's segments because they may overwrite or refer to the modules' spaces.
    PhaseClock clock;
    std::vector<ModuleTableItem> moduleTable;
    errorResult = readModuleTable(loadFile, &fullHeader, moduleTable);
    if (errorResult != NULL)
        return false;
    std::vector<ModuleId> levelModules;
    for (std::vector<ModuleTableItem>::const_iterator m = moduleTable.cbegin(); m != moduleTable.cend(); m++)
    {
        levelModules.push_back(m->modId);
        if (loadedModuleIds.count(ModuleKey(m->modId)) != 0)
            continue;
        const char* modError = NULL;
        int modErrNumber = 0;
        PolyObject* root;
        ModuleId loadedId;
        if (!loadModuleFile(m->path, &m->modId, modError, modErrNumber, root, loadedId))
        {
            errorBuffer = "Required module missing or changed (";
            errorBuffer += modError;
            errorBuffer += "): ";
            {
                std::vector<char> path(m->path.size() * sizeof(TCHAR) + 1);
#if (defined(_WIN32) && defined(UNICODE))
                sprintf(path.data(), "%S", m->path.c_str());
#else
                sprintf(path.data(), "%s", m->path.c_str());
#endif
                errorBuffer += path.data();
            }
            errorResult = errorBuffer.c_str();
            errNumber = 0;
            return false;
        }
    }

    clock.Lap("load-modules");

    // Now have a valid, matching saved state.
    // Load the segment descriptors.
    std::vector<SavedStateSegmentDescr> descrs;
    descrs.reserve(header.segmentDescrCount);
    if (fseek(loadFile, header.segmentDescr, SEEK_SET) != 0)
    {
        errorResult = "Unable to read segment descriptors";
        return false;
    }
    for (unsigned i = 0; i < header.segmentDescrCount; i++)
    {
        SavedStateSegmentDescr msd;
        if (fread(&msd, sizeof(SavedStateSegmentDescr), 1, loadFile) != 1)
        {
            errorResult = "Unable to read segment descriptors";
            return false;
        }
        descrs.push_back(msd);
    }

    std::vector<StateLoadData> loadData;
    loadData.reserve(header.segmentDescrCount);

#ifdef POLYML_MAP_SAVED_STATES
    // Only segments wholly within the file are mapped: a page past the end of a truncated
    // file would raise SIGBUS when touched.  The read path below reports the short read.
    struct stat fileStat;
    uint64_t fileSize = fstat(fileno(loadFile), &fileStat) == 0 ? (uint64_t)fileStat.st_size : 0;
#endif
    // Read in and create the new segments first.  If we have problems,
    // in particular if we have run out of memory, then it's easier to recover.  
    for (std::vector<SavedStateSegmentDescr>::iterator descr = descrs.begin(); descr < descrs.end(); descr++)
    {
        MemSpace *space = gMem.SpaceForIndex(descr->segmentIndex, descr->moduleId);
        StateLoadData load;
        load.relocationCount = descr->relocationCount;
        load.relocations = descr->relocations;
        if (space != NULL) // An existing segment.  If it's mutable we might be overwriting it.
            load.targetAddr = space->bottom;

        if (descr->segmentData == 0)
        { // No data - just an entry in the index.
            if (space == NULL/* ||
                descr->segmentSize != (size_t)((char*)space->top - (char*)space->bottom)*/)
            {
                errorResult = "Mismatch for existing memory space";
                return false;
            }
        }
        else if ((descr->segmentFlags & SSF_OVERWRITE) == 0)
        { // New segment.
            if (space != NULL)
            {
                errorResult = "Segment already exists";
                return false;
            }
            // Allocate memory for the new segment.
            unsigned mFlags =
                (descr->segmentFlags & SSF_WRITABLE ? MTF_WRITEABLE : 0) |
                (descr->segmentFlags & SSF_NOOVERWRITE ? MTF_NO_OVERWRITE : 0) |
                (descr->segmentFlags & SSF_BYTES ? MTF_BYTES : 0) |
                (descr->segmentFlags & SSF_CODE ? MTF_EXECUTABLE : 0);
            PermanentMemSpace *newSpace = 0;
#ifdef POLYML_MAP_SAVED_STATES
            // Map it where it was when saved.  If that address is taken, read it in instead
            // and let the relocations below adjust it and the pointers to it.
            if ((descr->segmentFlags & SSF_CODE) == 0 && descr->segmentSize <= fileSize &&
                    (uint64_t)descr->segmentData <= fileSize - descr->segmentSize)
                newSpace = gMem.MapPermanentSpace(descr->originalAddr, descr->segmentSize,
                    fileno(loadFile), descr->segmentData, mFlags, descr->segmentIndex, descr->moduleId);
#endif
            if (newSpace == 0)
            {
                // Code goes where it was when saved if that is free.
                newSpace = gMem.AllocateNewPermanentSpace(descr->segmentSize, mFlags, descr->segmentIndex, descr->moduleId,
                    descr->segmentFlags & SSF_CODE ? descr->originalAddr : 0);
                if (newSpace == 0)
                {
                    errorResult = "Unable to allocate memory";
                    return false;
                }

                PolyWord *mem  = newSpace->bottom;
                PolyWord* writeAble = newSpace->writeAble(mem);
                if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0)
                {
                    errorResult = "Unable to seek segment";
                    return false;
                }
                if (readData(writeAble, descr->segmentSize, loadFile) != 1)
                {
                    errorResult = "Unable to read segment";
                    return false;
                }
                // Fill unused space to the top of the area.
                gMem.FillUnusedSpace(writeAble +descr->segmentSize/sizeof(PolyWord),
                    newSpace->spaceSize() - descr->segmentSize/sizeof(PolyWord));
                // Leave it writable until we've done the relocations.
            }

            load.targetAddr = newSpace->bottom;
        }
        loadData.push_back(load);
    }

    clock.Lap("load-segments");

    // Now read in the mutable overwrites.  This overwrites the muable areas with the values from the saved state.
    for (std::vector<SavedStateSegmentDescr>::iterator descr = descrs.begin(); descr < descrs.end(); descr++)
    {
        if (descr->segmentFlags & SSF_OVERWRITE)
        {
            MemSpace* space = gMem.SpaceForIndex(descr->segmentIndex, descr->moduleId);
            if (space == NULL || descr->segmentSize > (size_t)((char*)space->top - (char*)space->bottom))
            {
                errorResult = "Mismatch for existing memory space";
                return false;
            }
            if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0 ||
                fread(space->bottom, descr->segmentSize, 1, loadFile) != 1)
            {
                errorResult = "Unable to read segment";
                return false;
            }
        }
    }

    clock.Lap("load-overwrite");

    // Now the relocations.  The saved data holds the values it had when saved, so an
    // entry needs applying only if the segment it is in or the one it points into is
    // no longer where it was then.
    std::vector<bool> moved(descrs.size());
    bool anyMoved = false;
    size_t movedCount = 0, ownCount = 0, ownMoved = 0;
    uint64_t relocsRead = 0, relocsApplied = 0;
    for (size_t j = 0; j < descrs.size(); j++)
    {
        moved[j] = (void*)loadData[j].targetAddr != descrs[j].originalAddr;
        anyMoved = anyMoved || moved[j];
        if (moved[j]) movedCount++;
        // Segments this file creates, rather than ones in a parent or the executable,
        // including those whose contents it overwrites.
        if (descrs[j].segmentData != 0 && (descrs[j].segmentFlags & SSF_OVERWRITE) == 0)
        {
            ownCount++;
            if (moved[j]) ownMoved++;
        }
    }
    // The segments by their original address, for relocating segments without relocations.
    std::vector<std::pair<uintptr_t, size_t> > byAddress;
    if (anyMoved)
    {
        for (size_t j = 0; j < descrs.size(); j++)
            if (descrs[j].segmentSize != 0)
                byAddress.push_back(std::make_pair((uintptr_t)descrs[j].originalAddr, j));
        std::sort(byAddress.begin(), byAddress.end());
    }
    std::vector<RelocationEntry> relocBuffer(anyMoved ? 4096 : 0);
    for (size_t j = 0; j < loadData.size(); j++)
    {
        StateLoadData *i = &loadData[j];
        PolyWord* baseAddr = i->targetAddr;
        ASSERT(baseAddr != NULL); // We should have created it.
        // Process explicit relocations.
        if (i->relocations && anyMoved)
        {
            if (fseek(loadFile, i->relocations, SEEK_SET) != 0)
            {
                errorResult = "Unable to read relocation segment";
                return false;
            }
            for (unsigned k = 0; k < i->relocationCount; )
            {
                size_t n = i->relocationCount - k;
                if (n > relocBuffer.size()) n = relocBuffer.size();
                if (fread(relocBuffer.data(), sizeof(RelocationEntry), n, loadFile) != n)
                {
                    errorResult = "Unable to read relocation segment";
                    return false;
                }
                for (size_t r = 0; r < n; r++)
                {
                    RelocationEntry &reloc = relocBuffer[r];
                    if (!moved[j] && !moved[reloc.targetSegment])
                        continue;
                    byte *setAddress = (byte*)baseAddr + reloc.relocAddress;
                    byte *targetAddress = (byte*)loadData[reloc.targetSegment].targetAddr + reloc.targetAddress;
                    ScanAddress::SetConstantValue(setAddress, (PolyObject*)(targetAddress), reloc.relKind);
                    relocsApplied++;
                }
                relocsRead += n;
                k += (unsigned)n;
            }
        }
        else if (anyMoved && descrs[j].segmentData != 0 && (descrs[j].segmentFlags & (SSF_BYTES | SSF_CODE)) == 0)
        {
            const char *error = relocateByScanning(baseAddr, descrs[j].segmentSize, descrs, byAddress, loadData, moved, relocsApplied);
            if (error != 0)
            {
                errorResult = error;
                return false;
            }
        }
    }

    // Set external references and clear weak refs now that the relocations can't overwrite them.
    for (size_t j = 0; j < descrs.size(); j++)
    {
        if (descrs[j].segmentData != 0 && (descrs[j].segmentFlags & (SSF_OVERWRITE | SSF_NOOVERWRITE)) == SSF_NOOVERWRITE)
        {
            ClearVolatile cwbr;
            cwbr.ScanAddressesInRegion(loadData[j].targetAddr, loadData[j].targetAddr + descrs[j].segmentSize / sizeof(PolyWord));
        }
    }

    clock.Lap("load-relocs");
    if (anyMoved)
        movedStateLoads++;
    if (debugOptions & (DEBUG_SAVING | DEBUG_SAVETIME))
        Log(
#if (defined(_WIN32) && defined(UNICODE))
            "LOAD: %S: "
#else
            "LOAD: %s: "
#endif
            "%zu of %zu segments moved (%zu of the %zu this file creates), %llu relocations read, %llu applied; "
            "loads with moved segments: %u states, %u modules\n",
            (TCHAR*)thisFile, movedCount, descrs.size(), ownMoved, ownCount, (unsigned long long)relocsRead,
            (unsigned long long)relocsApplied, movedStateLoads, movedModuleLoads);

    // Add an entry to the hierarchy table for this file.
    HierarchyTable level(thisFile, header.timeStamp);
    level.modules = levelModules;
    hierarchyTable.push_back(level);

    return true; // Succeeded
}

static void LoadState(TaskData *taskData, bool isHierarchy, Handle hFileList)
// Load a saved state or a hierarchy.  
// hFileList is a list if this is a hierarchy and a single name if it is not.
{
    StateLoader loader(isHierarchy, hFileList);
    // Request the main thread to do the load.  This may set the error string if it failed.
    processes->MakeRootRequest(taskData, &loader);

    if (loader.errorResult != 0)
    {
        if (loader.errNumber == 0)
            raise_fail(taskData, loader.errorResult);
        else
        {
            AutoFree<char*> buff((char *)malloc(strlen(loader.errorResult) + 2 + _tcslen(loader.fileName) * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
            sprintf(buff, "%s: %S", loader.errorResult, (TCHAR *)loader.fileName);
#else
            sprintf(buff, "%s: %s", loader.errorResult, (TCHAR *)loader.fileName);
#endif
            raise_syscall(taskData, buff, loader.errNumber);
        }
    }
}

// Load a saved state file and any ancestors.
POLYUNSIGNED PolyLoadState(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);

    try {
        LoadState(taskData, false, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

// Load hierarchy.  This provides a complete list of children and parents.
POLYUNSIGNED PolyLoadHierarchy(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);

    try {
         LoadState(taskData, true, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

/*
 *  Additional functions to provide information or change saved-state files.
 */

// These functions do not affect the global state so can be executed by
// the ML threads directly.

static Handle moduleIdAsByteVector(TaskData* taskData, struct _moduleId modId);

static Handle GetHierarchy(TaskData *taskData)
// Return the list of files and module IDs in the hierarchy.
{
    Handle saved = taskData->saveVec.mark();
    Handle list  = SAVE(ListNull);

    // Process this in reverse order.
    for (std::vector<HierarchyTable>::reverse_iterator i = hierarchyTable.rbegin(); i != hierarchyTable.rend(); i++)
    {
        Handle fName = SAVE(C_string_to_Poly(taskData, i->fileName.c_str()));
        Handle modId = moduleIdAsByteVector(taskData, i->timeStamp);
        Handle value = alloc_and_save(taskData, 2);
        value->WordP()->Set(0, modId->Word());
        value->WordP()->Set(1, fName->Word());
        Handle next  = alloc_and_save(taskData, sizeof(ML_Cons_Cell)/sizeof(PolyWord));
        DEREFLISTHANDLE(next)->h = value->Word();
        DEREFLISTHANDLE(next)->t = list->Word();
        taskData->saveVec.reset(saved);
        list = SAVE(next->Word());
    }
    return list;
}

// Return the file names and module IDs.
POLYUNSIGNED PolyGetHierarchy(POLYUNSIGNED threadId)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle result = 0;

    try {
        result = GetHierarchy(taskData);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

static void RenameParent(TaskData *taskData, PolyWord childName, PolyWord parentName)
// Change the name of the immediate parent stored in a child
{
    // The name of the file to modify.
    AutoFree<TCHAR*> fileNameBuff(Poly_string_to_T_alloc(childName));
    if (fileNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    // The new parent name to insert.
    AutoFree<TCHAR*> parentNameBuff(Poly_string_to_T_alloc(parentName));
    if (parentNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    AutoClose loadFile(_tfopen(fileNameBuff, _T("r+b"))); // Open for reading and writing
    if ((FILE*)loadFile == NULL)
    {
        AutoFree<char*> buff((char *)malloc(23 + _tcslen(fileNameBuff) * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
        sprintf(buff, "Cannot open load file: %S", (TCHAR *)fileNameBuff);
#else
        sprintf(buff, "Cannot open load file: %s", (TCHAR *)fileNameBuff);
#endif
        raise_syscall(taskData, buff, ERRORNUMBER);
    }

    SavedStateHeader fullHeader;
    // Read the header and check the signature.
    const char* headerError = readStateHeader(loadFile, &fullHeader);
    if (headerError != NULL)
        raise_fail(taskData, headerError);
    SavedStateHeaderV4& header = fullHeader.v4;

    // Does this actually have a parent?
    if (header.parentNameEntry == 0)
        raise_fail(taskData, "File does not have a parent");

    // At the moment the only entry in the string table is the parent
    // name so we can simply write a new one on the end of the file.
    // This makes the file grow slightly each time but it shouldn't be
    // significant.
    fseek(loadFile, 0, SEEK_END);
    header.stringTable = ftell(loadFile); // Remember where this is
    _fputtc(0, loadFile); // First byte of string table is zero
    _fputts(parentNameBuff, loadFile);
    _fputtc(0, loadFile); // A terminating null.
    header.stringTableSize = (_tcslen(parentNameBuff) + 2)*sizeof(TCHAR);

    // Now rewind and write the header with the revised string table.
    fseek(loadFile, 0, SEEK_SET);
    fwrite(&fullHeader, header.headerLength, 1, loadFile);
}

POLYUNSIGNED PolyRenameParent(POLYUNSIGNED threadId, POLYUNSIGNED childName, POLYUNSIGNED parentName)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();

    try {
        RenameParent(taskData, PolyWord::FromUnsigned(childName), PolyWord::FromUnsigned(parentName));
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

static Handle ShowParent(TaskData *taskData, Handle hFileName)
// Return the name of the immediate parent stored in a child
{
    AutoFree<TCHAR*> fileNameBuff(Poly_string_to_T_alloc(hFileName->Word()));
    if (fileNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    AutoClose loadFile(_tfopen(fileNameBuff, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        AutoFree<char*> buff((char *)malloc(23 + _tcslen(fileNameBuff) * sizeof(TCHAR) + 1));
        if (buff == NULL)
            raise_syscall(taskData, "Insufficient memory", NOMEMORY);
#if (defined(_WIN32) && defined(UNICODE))
        sprintf(buff, "Cannot open load file: %S", (TCHAR *)fileNameBuff);
#else
        sprintf(buff, "Cannot open load file: %s", (TCHAR *)fileNameBuff);
#endif
        raise_syscall(taskData, buff, ERRORNUMBER);
    }

    SavedStateHeader fullHeader;
    // Read the header and check the signature.
    const char* headerError = readStateHeader(loadFile, &fullHeader);
    if (headerError != NULL)
        raise_fail(taskData, headerError);
    SavedStateHeaderV4& header = fullHeader.v4;

    // Does this have a parent?
    if (header.parentNameEntry != 0)
    {
        size_t toRead = header.stringTableSize-header.parentNameEntry;
        size_t elems = ((toRead + sizeof(TCHAR) - 1) / sizeof(TCHAR));
        // Always allow space for null terminator
        size_t roundedBytes = (elems + 1) * sizeof(TCHAR);
        AutoFree<TCHAR*> parentFileName((TCHAR *)malloc(roundedBytes));
        if (parentFileName == NULL)
            raise_syscall(taskData, "Insufficient memory", NOMEMORY);

        if (header.parentNameEntry >= header.stringTableSize /* Bad entry */ ||
            fseek(loadFile, header.stringTable + header.parentNameEntry, SEEK_SET) != 0 ||
            fread(parentFileName, 1, toRead, loadFile) != toRead)
        {
            raise_fail(taskData, "Unable to read parent file name");
        }
        parentFileName[elems] = 0; // Should already be null-terminated, but just in case.
        // Convert the name into a Poly string and then build a "Some" value.
        // It's possible, although silly, to have the empty string as a parent name.
        Handle resVal = SAVE(C_string_to_Poly(taskData, parentFileName));
        Handle result = alloc_and_save(taskData, 1);
        DEREFHANDLE(result)->Set(0, resVal->Word());
        return result;
    }
    else return SAVE(NONE_VALUE);
}

// Return the name of the immediate parent stored in a child
POLYUNSIGNED PolyShowParent(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = ShowParent(taskData, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

// Return the module table of a saved state: the modules it depends on that its parents don't record.
static Handle ShowModuleDependencies(TaskData *taskData, Handle hFileName)
{
    AutoFree<TCHAR*> fileNameBuff(Poly_string_to_T_alloc(hFileName->Word()));
    if (fileNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    AutoClose loadFile(_tfopen(fileNameBuff, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        AutoFree<char*> buff((char *)malloc(23 + _tcslen(fileNameBuff) * sizeof(TCHAR) + 1));
        if (buff == NULL)
            raise_syscall(taskData, "Insufficient memory", NOMEMORY);
#if (defined(_WIN32) && defined(UNICODE))
        sprintf(buff, "Cannot open load file: %S", (TCHAR *)fileNameBuff);
#else
        sprintf(buff, "Cannot open load file: %s", (TCHAR *)fileNameBuff);
#endif
        raise_syscall(taskData, buff, ERRORNUMBER);
    }

    SavedStateHeader header;
    const char* error = readStateHeader(loadFile, &header);
    if (error != NULL)
        raise_fail(taskData, error);
    std::vector<ModuleTableItem> table;
    error = readModuleTable(loadFile, &header, table);
    if (error != NULL)
        raise_fail(taskData, error);

    // Build the list in reverse so it is in table order.
    Handle reset = taskData->saveVec.mark();
    Handle list = SAVE(ListNull);
    for (std::vector<ModuleTableItem>::reverse_iterator i = table.rbegin(); i != table.rend(); i++)
    {
        Handle idHandle = moduleIdAsByteVector(taskData, i->modId);
        Handle nameHandle = SAVE(C_string_to_Poly(taskData, i->path.c_str()));
        Handle pairHandle = alloc_and_save(taskData, 2);
        pairHandle->WordP()->Set(0, idHandle->Word());
        pairHandle->WordP()->Set(1, nameHandle->Word());
        Handle next = alloc_and_save(taskData, sizeof(ML_Cons_Cell) / sizeof(PolyWord));
        DEREFLISTHANDLE(next)->h = pairHandle->Word();
        DEREFLISTHANDLE(next)->t = list->Word();
        // Reset the save vec to keep it bounded.
        taskData->saveVec.reset(reset);
        list = SAVE(next->Word());
    }
    return list;
}

POLYUNSIGNED PolyShowModuleDependencies(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = ShowModuleDependencies(taskData, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

// Module system
#define MODULESIGNATURE "POLYMODU"
#define MODULEVERSION   4

typedef struct _moduleHeader
{
    // These entries are primarily to check that we have a valid
    // saved state file before we try to interpret anything else.
    char        headerSignature[8];     // Should contain MODULESIGNATURE
    unsigned    headerVersion;          // Should contain MODULEVERSION
    unsigned    headerLength;           // Number of bytes in the header
    unsigned    segmentDescrLength;     // Number of bytes in a descriptor

    // These entries contain the real data.
    off_t       segmentDescr;           // Position of segment descriptor table
    unsigned    segmentDescrCount;      // Number of segment descriptors in the table

    struct _moduleId thisModuleId;      // The id for this module.
    struct _moduleId executableModId;    // The id for the parent executable.

    off_t       stringTable;            // Pointer to the string table (zero if none)
    size_t      stringTableSize;        // Size of string table

    off_t       dependencies;           // Location of dependency table, if any
    unsigned    dependencyCount;

    // Root
    uintptr_t   rootSegment;
    POLYUNSIGNED     rootOffset;
} ModuleHeader;

// Entry for segment table.  This describes the segments on the disc that
// need to be loaded into memory.
typedef struct _moduleSegmentDescr
{
    off_t       segmentData;            // Position of the segment data
    size_t      segmentSize;            // Size of the segment data
    off_t       relocations;            // Position of the relocation table
    unsigned    relocationCount;        // Number of entries in relocation table
    unsigned    relocationSize;         // Size of a relocation entry
    unsigned    segmentFlags;           // Segment flags (see MSF_ values)
    unsigned    segmentIndex;           // The index of this segment or the segment it overwrites
    struct _moduleId moduleIden;               // The module this came from.
    // For a segment in another module or the executable, its address when this module was
    // written.  For this module's segment, the address its data was prelinked for, or 0 if it
    // was not: a module's immutable non-code data is prelinked when it is one segment.
    // A prelinked segment's data holds the pointer values its relocations would set, given
    // these addresses, so it can be mapped from the file there with no change.
    uintptr_t   originalAddr;
    // The relocations are sorted by target segment.  The table starts with relocRunCount
    // entries of ModRelocationRun, one for each run of relocations to a segment.
    unsigned    relocRunCount;
} ModuleSegmentDescr;

typedef struct _modRelocationRun
{
    unsigned    targetSegment;
    unsigned    count;
} ModRelocationRun;

// The region modules are prelinked for: 16TB above the saved-state region, in 64K slots.
// A module's segment starts in the slot its id selects, at its file offset modulo 64K so
// that it can be mapped on 4K, 16K and 64K pages.
#define MODULE_REGION_BASE  ((uintptr_t)0x300000000000)
#define MODULE_SLOT_BITS    28
#define MODULE_SLOT_SIZE    ((uintptr_t)0x10000)

static uintptr_t ModulePrelinkAddress(ModuleId m, uint64_t fileOffset)
{
    uint64_t slot = (ModuleKey(m) * 0x9E3779B97F4A7C15ull) >> (64 - MODULE_SLOT_BITS);
    return MODULE_REGION_BASE + (uintptr_t)slot * MODULE_SLOT_SIZE + (uintptr_t)(fileOffset % MODULE_SLOT_SIZE);
}

#define MSF_WRITABLE    1               // The segment contains mutable data
#define MSF_OVERWRITE   2               // The segment overwrites the data (mutable) in a parent.
#define MSF_NOOVERWRITE 4               // The segment must not be further overwritten
#define MSF_BYTES       8               // The segment contains only byte data
#define MSF_CODE        16              // The segment contains only code

typedef struct _modrelocationEntry
{
    // Each entry indicates a location that has to be set to an address.
    // The location to be set is determined by adding "relocAddress" to the base address of
    // this segment (the one to which these relocations apply) and the value to store
    // by adding "targetAddress" to the base address of the segment indicated by "targetSegment".
    POLYUNSIGNED    relocAddress;       // The (byte) offset in this segment that we will set
    POLYUNSIGNED    targetAddress;      // The value to add to the base of the destination segment
    unsigned        targetSegment;      // Index into the address table
    ScanRelocationKind relKind;         // The kind of relocation (processor dependent).
} ModRelocationEntry;

// Entry for a dependency.  This is really a guide to help to load the dependencies
// automatically.  The segment table is used to check that the memory segment needed
// have actually been loaded.
typedef struct _modDependencyEntry
{
    unsigned            depNameEntry;   // Index into the string table for the name
    struct _moduleId    depId;          // Id for this dependency
} ModDependencyEntry;

class ModuleExporter : public MainThreadRequest, public StateExport
{
public:
    ModuleExporter(const TCHAR* file, Handle r) :
        MainThreadRequest(MTP_STOREMODULE), fileName(file), root(r), relocationCount(0) {
    }

    virtual void Perform();

    const TCHAR* fileName;
    Handle root;

    std::vector<ModIdAndName> dependencies;

    void RunModuleExport(PolyObject* rootFunction);
    virtual void exportStore(void) {}

protected:
    void createActualRelocation(void* addr, void* relocAddr, ScanRelocationKind kind);
    unsigned relocationCount;
    std::vector<ModRelocationEntry> relocations; // For the segment being written.
#ifndef _WIN32
    // The module is written to tempName and renamed to publishName when it is complete.
    std::string publishName, tempName;
#endif

    friend class SaveRequest;
};

// Create a relocation entry for an address at a given location.
// In compact-32 bit mode this will only be called for modules.
void ModuleExporter::createActualRelocation(void* addr, void* relocAddr, ScanRelocationKind kind)
{
    ModRelocationEntry reloc;
    // Set the offset within the section we're scanning.
    setRelocationAddress(relocAddr, &reloc.relocAddress);

    unsigned addrArea = findArea(addr);
    reloc.targetAddress = (POLYUNSIGNED)((char*)addr - (char*)memTable[addrArea].mtOriginalAddr);
    reloc.targetSegment = addrArea;
    reloc.relKind = kind;
    relocations.push_back(reloc);
    relocationCount++;
}

static bool lessTargetSegment(const ModRelocationEntry& a, const ModRelocationEntry& b)
{
    return a.targetSegment < b.targetSegment;
}

static void storeAddress(byte* addressToWrite, uintptr_t valu)
{
    for (unsigned i = 0; i < sizeof(uintptr_t); i++)
    {
        addressToWrite[i] = (byte)(valu & 255);
        valu >>= 8;
    }
}

// The number of bytes a relocation of this kind sets.
static size_t RelocationSize(ScanRelocationKind kind)
{
    switch (kind)
    {
    case PROCESS_RELOC_DIRECT: return sizeof(uintptr_t);
    case PROCESS_RELOC_I386RELATIVE: case PROCESS_RELOC_C32ADDR: return 4;
    default: return 8; // An ARM64 instruction pair.
    }
}

// Zero the bits a relocation of this kind sets, which depend on where things are in
// memory, and keep the rest: in an ARM64 instruction pair, the opcodes and registers.
static void ZeroRelocatedBits(byte* p, ScanRelocationKind kind)
{
    switch (kind)
    {
    case PROCESS_RELOC_ARM64ADRPLDR64:
    case PROCESS_RELOC_ARM64ADRPLDR32:
    case PROCESS_RELOC_ARM64ADRPADD:
    {
        // As ScanAddress::SetConstantValue sets them.
        uint32_t instr[2];
        memcpy(instr, p, sizeof(instr));
        instr[0] = toARMInstr(fromARMInstr(instr[0]) & 0x9f00001f);
        instr[1] = toARMInstr(fromARMInstr(instr[1]) & 0xffc003ff);
        memcpy(p, instr, sizeof(instr));
        break;
    }
    default:
        memset(p, 0, RelocationSize(kind));
    }
}

// A hash of a module's content and layout.  It has no time or sequence seed, so
// the same module written by any process gets the same id.
class ModuleHash
{
public:
    ModuleHash() : h1(0x9E3779B97F4A7C15ull), h2(0xC2B2AE3D27D4EB4Full), count(0) {}

    void addWord(uint64_t w)
    {
        h1 = rotl((h1 ^ w) * 0x87C37B91114253D5ull, 31);
        h2 = rotl((h2 ^ w) * 0x4CF5AD432745937Full, 33);
        count++;
    }

    void addBytes(const byte* p, size_t n)
    {
        size_t i = 0;
        for (; i + sizeof(uint64_t) <= n; i += sizeof(uint64_t))
        {
            uint64_t w;
            memcpy(&w, p + i, sizeof(w));
            addWord(w);
        }
        uint64_t tail = 0;
        memcpy(&tail, p + i, n - i);
        addWord(tail);
        addWord(n);
    }

    struct _moduleId result() const
    {
        uint64_t h = mix(mix(h1 ^ count) + h2);
        if (h == 0) h = 1;
        struct _moduleId id = { (uint32_t)h, (uint32_t)(h >> 32) };
        return id;
    }

private:
    static uint64_t rotl(uint64_t x, unsigned k) { return (x << k) | (x >> (64 - k)); }
    static uint64_t mix(uint64_t x)
    {
        x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull;
        x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull;
        x ^= x >> 33;
        return x;
    }
    uint64_t h1, h2, count;
};

// This is called by the initial thread to actually do the export.
void ModuleExporter::RunModuleExport(PolyObject* rootFn)
{
    PolyObject* copiedRoot = 0;
    CopyScan copyScan;
    copyScan.singleImmutableArea = true;

    // Set the dependencies.  These are included first since the caller may have provided names.
    copyScan.dependencies[exportSignature] = true; // Always include the executable
    for (std::vector<ModIdAndName>::iterator i = dependencies.begin(); i < dependencies.end(); i++)
        copyScan.dependencies[i->modId] = true;
    // Add any further dependencies to produce the transitive closure.
    // Can't use an iterator here because the size may increase.  Actually, every dependency will
    // already include the transitive closure of its dependencies so it isn't necessary to look
    // deeper.
    for (std::vector<ModIdAndName>::size_type i = 0; i < dependencies.size(); i++)
    {
        if (dependencies[i].isModule)
        {
            ModuleId m = dependencies[i].modId;
            for (std::vector<LoadedModuleData>::const_iterator j = loadedModules.cbegin(); j != loadedModules.cend(); j++)
            {
                // Find the entry in the loaded module table.  It should be there somewhere.
                if (j->modId == m)
                {
                    // Add all the dependencies if they're not already there.
                    for (std::vector<ModuleId>::const_iterator k = j->modDependencies.cbegin(); k < j->modDependencies.cend(); k++)
                    {
                        if (!copyScan.dependencies[k->modId])
                        {
                            // A module exported over a saved state records the state's level, which
                            // the loop below expands with its parents and recorded modules.
                            bool isLevel = false;
                            for (std::vector<HierarchyTable>::const_iterator h = hierarchyTable.cbegin(); h != hierarchyTable.cend(); h++)
                                if (*k == ModuleId(h->timeStamp))
                                    isLevel = true;
                            ModIdAndName m(_T(""), k->modId, !isLevel);
                            // Add this to the end.  It will be processed later but the sub-dependencies should already be there.
                            dependencies.push_back(m);
                            copyScan.dependencies[k->modId] = true; // Don't need to add it again.
                        }
                    }

                    break; // Don't need to search further
                }
            }
        }
        else
        {
            ModuleId m = dependencies[i].modId;
            // Add all the saved states at this and higher levels since any data in these levels should be
            // referenced rather than copied.  The entry should be there because we've already checked.
            for (std::vector <HierarchyTable>::iterator i = hierarchyTable.begin(); i < hierarchyTable.end(); i++)
            {
                copyScan.dependencies[i->timeStamp] = true;
                // The modules a level recorded are loaded with it, so their data is referenced too.
                for (std::vector<ModuleId>::const_iterator r = i->modules.cbegin(); r != i->modules.cend(); r++)
                    if (loadedModuleIds.count(ModuleKey(*r)) != 0)
                        copyScan.dependencies[*r] = true;
                if (m == i->timeStamp)
                    break;
            }
        }
    }

    PhaseClock clock;
    try {
        copyScan.initialise();
        // Copy the root and everything reachable from it into the temporary area.
        copiedRoot = copyScan.ScanObjectAddress(rootFn);
        clock.Lap("module-scan");
        // Fix the permanent areas.  We may have copied cells from modules if they are
        // not dependencies.
        CopyScan::fixPermanentAreas();
        clock.Lap("module-fixperm");
    }
    catch (MemoryException&)
    {
        errorMessage = "Insufficient Memory";
        // Reset the forwarding pointers so that the original data structure is in
        // local storage.
        CopyScan::revertToLocal();
        return;
    }
    catch (std::bad_alloc&)
    {
        errorMessage = "Insufficient Memory : C++ allocation failed";
        CopyScan::revertToLocal();
        return;
    }

    // Provisional: the id is computed from the module's content below.
    exportModId = copyScan.extractHash();

    // Copy the areas into the export object.
    size_t tableSize = gMem.eSpaces.size() + gMem.pSpaces.size();
    unsigned memEntry = 0;
    try {
        memTable = new ExportMemTable[tableSize];
    }
    catch (std::bad_alloc&)
    {
        errorMessage = "Insufficient Memory : C++ allocation failed";
        CopyScan::revertToLocal();
        return;
    }

    // If we're constructing a module we need to include the global spaces.
    // Permanent spaces from the dependencies
    for (std::vector<PermanentMemSpace*>::iterator i = gMem.pSpaces.begin(); i < gMem.pSpaces.end(); i++)
    {
        PermanentMemSpace* space = *i;
        if (copyScan.dependencies[space->moduleIdentifier])
        {
            ExportMemTable* entry = &memTable[memEntry++];
            entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
            entry->mtLength = (space->topPointer - space->bottom) * sizeof(PolyWord);
            entry->mtIndex = space->index;
            entry->mtModId = space->moduleIdentifier;
            entry->mtFlags = 0;
            if (space->isMutable) entry->mtFlags |= MTF_WRITEABLE;
            if (space->isCode) entry->mtFlags |= MTF_EXECUTABLE;
        }
    }
    unsigned newAreas = memEntry;

    for (std::vector<PermanentMemSpace*>::iterator i = gMem.eSpaces.begin(); i < gMem.eSpaces.end(); i++)
    {
        ExportMemTable* entry = &memTable[memEntry++];
        PermanentMemSpace* space = *i;
        entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
        entry->mtLength = (space->topPointer - space->bottom) * sizeof(PolyWord);
        // The spaces need to be renumbered and the module id set.
        space->index = memEntry - newAreas - 1;
        space->moduleIdentifier = ModuleId(exportModId);
        entry->mtIndex = space->index;
        entry->mtModId = space->moduleIdentifier;
        entry->mtFlags = 0;
        if (space->isMutable)
        {
            entry->mtFlags = MTF_WRITEABLE;
            if (space->noOverwrite) entry->mtFlags |= MTF_NO_OVERWRITE;
        }
        if (space->isCode && !space->constArea) entry->mtFlags |= MTF_EXECUTABLE;
        if (space->byteOnly) entry->mtFlags |= MTF_BYTES;
    }

    ASSERT(memEntry <= tableSize);
    memTableEntries = memEntry;
    rootFunction = copiedRoot;

    // Write the data out.  This handles memory exceptions but we should also
    // handle write failures.  That could happen if the disc became full.
    try {
        ModuleHeader modHeader;
        memset(&modHeader, 0, sizeof(modHeader));
        modHeader.headerLength = sizeof(modHeader);
        memcpy(modHeader.headerSignature,
            MODULESIGNATURE, sizeof(modHeader.headerSignature));
        modHeader.headerVersion = MODULEVERSION;
        modHeader.segmentDescrLength = sizeof(ModuleSegmentDescr);
        modHeader.executableModId = exportSignature;
        {
            unsigned rootArea = findArea(this->rootFunction);
            ExportMemTable* mt = &memTable[rootArea];
            modHeader.rootSegment = rootArea;
            modHeader.rootOffset = (POLYUNSIGNED)((char*)this->rootFunction - (char*)mt->mtOriginalAddr);
        }

        // Collect the relocations of the new segments and hash them with the segments'
        // flags, sizes and data, the root and the dependencies.  The data is hashed with
        // the locations relocations set zeroed, and a relocation's target by its module and
        // segment index, so the hash does not depend on where anything is in memory.
        // Relocations are hashed in the order the segment's objects give them, not in the
        // order of the target segments, which depends on the order dependencies were loaded.
        // Data a load resets (ClearVolatile) is hashed zeroed: it can differ between writers.
        std::vector<std::vector<ModRelocationEntry> > segmentRelocations(this->memTableEntries);
        ModuleHash idHash;
        idHash.addWord(this->memTableEntries - newAreas);
        for (unsigned k = newAreas; k < this->memTableEntries; k++)
        {
            ExportMemTable* entry = &this->memTable[k];
            this->relocations.clear();
            char* start = (char*)entry->mtOriginalAddr;
            char* end = start + entry->mtLength;
            std::vector<byte> zeroed((byte*)start, (byte*)end);
            // The byte ranges of data a load resets, in order.
            std::vector<std::pair<size_t, size_t> > volatileRanges;
            for (PolyWord* p = (PolyWord*)start; p < (PolyWord*)end; )
            {
                p++;
                PolyObject* obj = (PolyObject*)p;
                POLYUNSIGNED length = obj->Length();
                // For saved states we don't include explicit relocations except
                // in code but it's easier if we do for modules.
                if (length != 0 && obj->IsCodeObject())
                    machineDependent->ScanConstantsWithinCode(obj, this);
                relocateObject(obj);
                if (obj->IsMutable() && obj->IsNoOverwriteObject())
                {
                    size_t offset = (char*)obj - start;
                    size_t bytes = obj->IsByteObject() && obj->IsWeakRefObject() ?
                        (length >= sizeof(uintptr_t) / sizeof(PolyWord) ? sizeof(uintptr_t) : 0) :
                        obj->IsByteObject() ? 0 : length * sizeof(PolyWord);
                    memset(zeroed.data() + offset, 0, bytes);
                    if (bytes != 0)
                        volatileRanges.push_back(std::make_pair(offset, offset + bytes));
                }
                p += length;
            }
            idHash.addWord(entry->mtFlags);
            for (std::vector<ModRelocationEntry>::iterator r = this->relocations.begin(); r < this->relocations.end(); r++)
            {
                ASSERT(r->relocAddress + RelocationSize(r->relKind) <= entry->mtLength);
                ZeroRelocatedBits(zeroed.data() + r->relocAddress, r->relKind);
                // A relocation in data a load resets doesn't affect the loaded module.
                std::vector<std::pair<size_t, size_t> >::const_iterator v =
                    std::upper_bound(volatileRanges.cbegin(), volatileRanges.cend(),
                        std::make_pair((size_t)r->relocAddress, ~(size_t)0));
                if (v != volatileRanges.cbegin() && (size_t)r->relocAddress < (v - 1)->second)
                    continue;
                const ExportMemTable* target = &this->memTable[r->targetSegment];
                idHash.addWord(r->relocAddress);
                idHash.addWord(r->targetSegment < newAreas ? ModuleKey(target->mtModId) : 0);
                idHash.addWord(target->mtIndex);
                idHash.addWord(r->targetAddress);
                idHash.addWord(r->relKind);
            }
            idHash.addBytes(zeroed.data(), zeroed.size());
            // The file groups the relocations by target segment.
            std::stable_sort(this->relocations.begin(), this->relocations.end(), lessTargetSegment);
            segmentRelocations[k].swap(this->relocations);
        }
        idHash.addWord(modHeader.rootSegment < newAreas ? ModuleKey(memTable[modHeader.rootSegment].mtModId) : 0);
        idHash.addWord(memTable[modHeader.rootSegment].mtIndex);
        idHash.addWord(modHeader.rootOffset);
        {
            // In a fixed order: the list's order depends on the order modules were loaded.
            std::vector<uint64_t> dependencyKeys;
            for (std::vector<ModIdAndName>::const_iterator i = dependencies.cbegin(); i != dependencies.cend(); i++)
                dependencyKeys.push_back(ModuleKey(i->modId));
            std::sort(dependencyKeys.begin(), dependencyKeys.end());
            for (std::vector<uint64_t>::const_iterator i = dependencyKeys.cbegin(); i != dependencyKeys.cend(); i++)
                idHash.addWord(*i);
        }
        exportModId = idHash.result();
        for (unsigned k = newAreas; k < this->memTableEntries; k++)
            this->memTable[k].mtModId = exportModId;
        for (std::vector<PermanentMemSpace*>::iterator i = gMem.eSpaces.begin(); i < gMem.eSpaces.end(); i++)
            (*i)->moduleIdentifier = ModuleId(exportModId);
        modHeader.thisModuleId = exportModId;
        // A module with the same content that is already loaded has this id, and exports
        // naming the id refer to its data, not to these objects: refuse to export them.
        if (loadedModuleIds.count(ModuleKey(exportModId)) != 0)
        {
            errorMessage = "A module with the same contents is already loaded";
            throw IOException();
        }
        modHeader.segmentDescrCount = this->memTableEntries; // One segment for each space.
        // Write out the header.
        if (!checkedFwrite(&modHeader, sizeof(modHeader), 1))
            throw IOException();

        std::vector<ModuleSegmentDescr> descrs;
        descrs.reserve(this->memTableEntries);
        // We need an entry in the descriptor tables for each segment in the executable because
        // we may have relocations that refer to addresses in it.
        for (unsigned j = 0; j < this->memTableEntries; j++)
        {
            ModuleSegmentDescr thisDescr;;
            ExportMemTable* entry = &this->memTable[j];
            memset(&thisDescr, 0, sizeof(ModuleSegmentDescr));
            thisDescr.relocationSize = sizeof(ModRelocationEntry);
            thisDescr.segmentIndex = entry->mtIndex;
            thisDescr.segmentSize = entry->mtLength; // Set this even if we don't write it.
            thisDescr.moduleIden = entry->mtModId;
            if (j < newAreas)
                thisDescr.originalAddr = (uintptr_t)entry->mtOriginalAddr;
            if (entry->mtFlags & MTF_WRITEABLE)
            {
                thisDescr.segmentFlags |= MSF_WRITABLE;
                if (entry->mtFlags & MTF_NO_OVERWRITE)
                    thisDescr.segmentFlags |= MSF_NOOVERWRITE;
                if ((entry->mtFlags & MTF_NO_OVERWRITE) == 0)
                    thisDescr.segmentFlags |= MSF_OVERWRITE;
                if (entry->mtFlags & MTF_BYTES)
                    thisDescr.segmentFlags |= MSF_BYTES;
            }
            if (entry->mtFlags & MTF_EXECUTABLE)
                thisDescr.segmentFlags |= MSF_CODE;
            descrs.push_back(thisDescr);
        }
        // Write out temporarily. Will be overwritten at the end.
        modHeader.segmentDescr = ftell(this->exportFile);
        if (!checkedFwrite(descrs.data(), sizeof(ModuleSegmentDescr), this->memTableEntries))
            throw IOException();

        // Write out the data and the relocations.
        unsigned immutableSegments = 0;
        for (unsigned k = newAreas; k < this->memTableEntries; k++)
            if (this->memTable[k].mtFlags == 0) immutableSegments++;
        for (unsigned k = newAreas; k < this->memTableEntries; k++)
        {
            bool prelink = immutableSegments == 1 && this->memTable[k].mtFlags == 0;
            ModuleSegmentDescr* thisDescr = &descrs[k];
            ExportMemTable* entry = &this->memTable[k];
            char* start = (char*)entry->mtOriginalAddr;
            char* end = start + entry->mtLength;
            this->relocations.swap(segmentRelocations[k]);
            std::vector<ModRelocationRun> runs;
            for (std::vector<ModRelocationEntry>::iterator r = this->relocations.begin(); r < this->relocations.end(); r++)
            {
                if (runs.empty() || runs.back().targetSegment != r->targetSegment)
                {
                    ModRelocationRun run = { r->targetSegment, 0 };
                    runs.push_back(run);
                }
                runs.back().count++;
                if (r->relKind != PROCESS_RELOC_DIRECT)
                    prelink = false;
            }
            thisDescr->relocationCount = (unsigned)this->relocations.size();
            thisDescr->relocRunCount = (unsigned)runs.size();
            // Write out the data.  A prelinked segment is written with its pointers set for
            // it at its prelink address and the other segments where they are now.
            thisDescr->segmentData = ftell(exportFile);
            if (prelink)
            {
                uintptr_t prelinkAddr = ModulePrelinkAddress(exportModId, (uint64_t)thisDescr->segmentData);
                std::vector<byte> staging((byte*)start, (byte*)end);
                for (std::vector<ModRelocationEntry>::iterator r = this->relocations.begin(); r < this->relocations.end(); r++)
                {
                    uintptr_t base = r->targetSegment == k ? prelinkAddr : (uintptr_t)this->memTable[r->targetSegment].mtOriginalAddr;
                    storeAddress(staging.data() + r->relocAddress, base + r->targetAddress);
                }
                thisDescr->originalAddr = prelinkAddr;
                if (!checkedFwrite(staging.data(), entry->mtLength, 1))
                    throw IOException();
            }
            else if (!checkedFwrite(entry->mtOriginalAddr, entry->mtLength, 1))
                throw IOException();
            thisDescr->relocations = ftell(this->exportFile);
            if (!runs.empty() && !checkedFwrite(runs.data(), sizeof(ModRelocationRun), runs.size()))
                throw IOException();
            if (!this->relocations.empty() &&
                    !checkedFwrite(this->relocations.data(), sizeof(ModRelocationEntry), this->relocations.size()))
                throw IOException();
            this->relocations.clear();
        }
        // Write the string table and construct the dependency table at the same time
        std::vector<ModDependencyEntry> dependencyTable;

        modHeader.stringTable = ftell(exportFile);
        unsigned stringPos = 0;
        _fputtc(0, exportFile); // First byte of string table is zero
        stringPos += sizeof(TCHAR);
        for (std::vector<ModIdAndName>::iterator i = dependencies.begin(); i < dependencies.end(); i++)
        {
            ModDependencyEntry depEntry;
            depEntry.depNameEntry = stringPos;
            _fputts(i->modName.c_str(), exportFile);
            stringPos += (unsigned)(i->modName.size() * sizeof(TCHAR));
            _fputtc(0, exportFile); // A terminating null.
            stringPos += sizeof(TCHAR);
            depEntry.depId = i->modId;
            dependencyTable.push_back(depEntry);
        }

        modHeader.stringTableSize = stringPos;
        if (ferror(exportFile))
        {
            errNumber = ERRORNUMBER;
            errorMessage = "Error while writing string table";
            throw IOException();
        }
        // And the dependency table itself if needed.
        if (!dependencyTable.empty())
        {
            modHeader.dependencies = ftell(exportFile);
            modHeader.dependencyCount = (unsigned)dependencyTable.size();
            if (!checkedFwrite(dependencyTable.data(), sizeof(ModDependencyEntry), dependencyTable.size()))
                throw IOException();
        }

        // Rewrite the header and the segment tables now they're complete.
        if (fseek(exportFile, 0, SEEK_SET) != 0)
        {
            errNumber = ERRORNUMBER;
            errorMessage = "Error while writing module";
            throw IOException();
        }
        if (!checkedFwrite(&modHeader, sizeof(modHeader), 1))
            throw IOException();
        if (!checkedFwrite(descrs.data(), sizeof(ModuleSegmentDescr), this->memTableEntries))
            throw IOException();

        int closed = fclose(exportFile);
        exportFile = NULL;
        if (closed != 0)
        {
            errNumber = ERRORNUMBER;
            errorMessage = "Error while writing module";
            throw IOException();
        }
#ifndef _WIN32
        if (rename(tempName.c_str(), publishName.c_str()) != 0)
        {
            errNumber = ERRORNUMBER;
            errorMessage = "Cannot rename module file";
            throw IOException();
        }
        tempName.clear();
#endif

        // Add this to the loaded module table.
        LoadedModuleData loadMod;
        loadMod.modId = exportModId;
        loadMod.modDependencies.reserve(dependencies.size());
        for (std::vector<ModIdAndName>::const_iterator i = dependencies.cbegin(); i != dependencies.cend(); i++)
            loadMod.modDependencies.push_back(i->modId);
        loadedModules.push_back(loadMod);
        try {
            loadedModuleIds.insert(ModuleKey(exportModId));
        }
        catch (...) {
            loadedModules.pop_back();
            throw;
        }

        // If it all succeeded we can switch over to the permanent spaces.  If promoting
        // them fails it changes nothing, and the module is not loaded.
        if (gMem.PromoteNewExportSpaces(exportModId))
            switchLocalsToPermanent();
        else
        {
            loadedModules.pop_back();
            loadedModuleIds.erase(ModuleKey(exportModId));
            errorMessage = "Insufficient Memory";
            CopyScan::revertToLocal();
        }

    }
    catch (IOException&)
    {
        CopyScan::revertToLocal();
    }
    catch (MemoryException&) {
        errorMessage = "Insufficient Memory";
        CopyScan::revertToLocal();
    }
    catch (std::bad_alloc&) {
        errorMessage = "Insufficient Memory : C++ allocation failed";
        CopyScan::revertToLocal();
    }
}

void ModuleExporter::Perform()
{
    // RunModuleExport copies everything reachable from the root, except data from
    // the executable because we've set the hierarchy to 1, using CopyScan.
    // It builds the tables in the export data structure then writes the data.
    if (!root->Word().IsDataPtr())
    {
        // If we have a completely empty module the list may be null.
        // This needs to be dealt with at a higher level.
        errorMessage = "Module root is not an address";
        return;
    }
    // A loaded module's file must not be replaced: states record modules by path.
    for (std::vector<LoadedModuleData>::const_iterator m = loadedModules.cbegin(); m != loadedModules.cend(); m++)
    {
        if (!m->fileName.empty() && sameFile(m->fileName.c_str(), fileName))
        {
            errorMessage = "File being saved is a loaded module";
            return;
        }
    }
    try {
#if (defined(_WIN32) && defined(UNICODE))
        exportFile = _wfopen(fileName, L"wb");
#elif defined(_WIN32)
        exportFile = fopen(fileName, "wb");
#else
        {
            // Other processes may have the existing file mapped or be reading it: write a
            // new file beside it and rename it over the name when it is complete.  As saving
            // a state does, write to the file a symbolic link names and keep its permissions.
            char *existing = realpath(fileName, NULL); // NULL if there's no file yet.
            publishName = existing != NULL ? existing : fileName;
            free(existing);
            // Created with the old file's permissions, or as a new file is, less the umask.
            // If the umask removed some of the old file's permissions, they are set with
            // fchmod, ignoring a failure: sandboxes may forbid it.
            struct stat oldStat;
            bool replacing = stat(publishName.c_str(), &oldStat) == 0;
            mode_t mode = replacing ? (oldStat.st_mode & 0777) : 0666;
            static unsigned long sequence = 0;
            int fd = -1;
            for (unsigned attempt = 0; fd < 0 && attempt < 100; attempt++)
            {
                // A short name in the same directory, so the rename stays atomic and a long
                // destination name doesn't make it too long.
                std::string::size_type slash = publishName.rfind('/');
                tempName = (slash == std::string::npos ? std::string() : publishName.substr(0, slash + 1)) +
                    ".pmo." + std::to_string((long)getpid()) + "." + std::to_string(sequence++);
                fd = open(tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
                if (fd < 0 && errno != EEXIST)
                    break;
            }
            if (fd < 0)
            {
                int err = errno;
                tempName.clear();
                errno = err;
            }
            else
            {
                struct stat newStat;
                if (replacing && fstat(fd, &newStat) == 0 && (newStat.st_mode & 0777) != mode)
                    (void)fchmod(fd, mode);
                if ((exportFile = fdopen(fd, "wb")) == NULL)
                {
                    int err = errno;
                    close(fd);
                    errno = err;
                }
            }
        }
#endif
        if (exportFile == NULL)
        {
            errorMessage = "Cannot open export file";
            errNumber = ERRORNUMBER;
        }
        else RunModuleExport(root->WordP());
    }
    catch (const std::bad_alloc&)
    {
        errorMessage = "Insufficient Memory : C++ allocation failed";
    }
#ifndef _WIN32
    // Not renamed: the export failed.
    if (!tempName.empty())
        unlink(tempName.c_str());
#endif
}

// Return a module Id as a string.  Also used in saved state.
static Handle moduleIdAsByteVector(TaskData* taskData, struct _moduleId modId)
{
    union {
        struct _moduleId mId;
        char chars[sizeof(struct _moduleId)];
    } asVector;
    asVector.mId = modId;
    // Convert the module Id to a vector of bytes.  The representation is the same as a string.
    return taskData->saveVec.push(C_string_to_Poly(taskData, asVector.chars, sizeof(struct _moduleId)));
}

static ModuleId moduleIdFromByteVector(TaskData* taskData, PolyWord p)
{
    union {
        struct _moduleId mId;
        char chars[sizeof(struct _moduleId)];
    } asVector;
    // Extract the module Id which is in Word8Vector.vector object.
    PolyStringObject* str = (PolyStringObject*)p.AsObjPtr();
    if (str->length != sizeof(struct _moduleId))
        raise_fail(taskData, "Incorrect format for module ID");
    memcpy(asVector.chars, str->chars, sizeof(struct _moduleId));
    return ModuleId(asVector.mId);
}

// Store a module
POLYUNSIGNED PolyStoreModule(POLYUNSIGNED threadId, POLYUNSIGNED filename, POLYUNSIGNED contents, POLYUNSIGNED depends)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedContents = taskData->saveVec.push(contents);
    Handle result = 0;

    try {
        TempString fileName(PolyWord::FromUnsigned(filename));
        ModuleExporter storer(fileName, pushedContents);

        // Process the list of dependencies.  It is a list of moduleId*string pairs.
        for (PolyWord l = PolyWord::FromUnsigned(depends); l.IsDataPtr(); l = ((ML_Cons_Cell*)l.AsObjPtr())->t)
        {
            PolyObject* p = ((ML_Cons_Cell*)l.AsObjPtr())->h.AsObjPtr();
            ModuleId mId = moduleIdFromByteVector(taskData, p->Get(0));
            // See if this is a module or a saved state.
            std::vector<LoadedModuleData>::iterator i = loadedModules.begin();
            while (i != loadedModules.end())
            {
                if (i->modId == mId)
                {
                    ModIdAndName mn(PolyStringToTString(p->Get(1)), mId, true);
                    storer.dependencies.push_back(mn);
                    break;
                }
                i++;
            }
            if (i == loadedModules.end())
            {
                std::vector<HierarchyTable>::iterator j = hierarchyTable.begin();
                while (j != hierarchyTable.end())
                {
                    if (ModuleId(j->timeStamp) == mId)
                    {
                        // Set the name to the empty string.  We cannot auto-load a saved state.
                        ModIdAndName mn(_T(""), mId, false);
                        storer.dependencies.push_back(mn);
                        break;
                    }
                    j++;
                }
                if (j == hierarchyTable.end())
                    raise_fail(taskData, "A dependency is listed but the module is not loaded");
            }
        }

        processes->MakeRootRequest(taskData, &storer);
        if (storer.errorMessage)
            raise_syscall(taskData, storer.errorMessage, storer.errNumber);

        result = moduleIdAsByteVector(taskData, storer.getExportId());
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

// Load a module.
class ModuleLoader : public MainThreadRequest
{
public:
    ModuleLoader(TaskData* taskData, const std_tstring file) :
        MainThreadRequest(MTP_LOADMODULE), callerTaskData(taskData), fileName(file),
        errorResult(NULL), errNumber(0), rootHandle(0) {
    }

    virtual void Perform();

    TaskData* callerTaskData;
    std_tstring fileName;
    const char* errorResult;
    int errNumber;
    Handle rootHandle;
    ModuleId loadedModId;
};

// Data needed for relocation during load.
class ModuleLoadData {
public:
    ModuleLoadData() : relocations(0), relocationCount(0), relocRunCount(0), targetAddr(0), moved(true) {}

    off_t       relocations;        // Copied from descriptor
    unsigned    relocationCount;
    unsigned    relocRunCount;
    PolyWord* targetAddr;           // Actual address of the segment
    bool        moved;              // Not at the descriptor's originalAddr
};

// Load a module file.  Must be called by the root thread.  If requiredId is not NULL the module
// must have that id.  Returns true and sets root and loadedId if it succeeded.  The caller is
// responsible for keeping the root reachable if it needs it.
static bool loadModuleFile(const std_tstring& fileName, const ModuleId* requiredId,
    const char*& errorResult, int& errNumber, PolyObject*& root, ModuleId& loadedId)
{
    {
        AutoClose loadFile(_tfopen(fileName.c_str(), _T("rb")));
        if ((FILE*)loadFile == NULL)
        {
            errorResult = "Cannot open load file";
            errNumber = ERRORNUMBER;
            return false;
        }

        ModuleHeader header;
        // Read the header and check the signature.
        if (fread(&header, sizeof(ModuleHeader), 1, loadFile) != 1)
        {
            errorResult = "Unable to load header";
            return false;
        }
        if (strncmp(header.headerSignature, MODULESIGNATURE, sizeof(header.headerSignature)) != 0)
        {
            errorResult = "File is not a Poly/ML module";
            return false;
        }
        if (header.headerVersion != MODULEVERSION ||
            header.headerLength != sizeof(ModuleHeader) ||
            header.segmentDescrLength != sizeof(ModuleSegmentDescr))
        {
            errorResult = "Unsupported version of module file";
            return false;
        }
        if (requiredId != NULL && ModuleId(header.thisModuleId) != *requiredId)
        {
            errorResult = "Module has a different id";
            return false;
        }
        // Check to see if this is already loaded
        if (loadedModuleIds.count(ModuleKey(header.thisModuleId)) != 0)
        {
            errorResult = "A module with this signature is already loaded";
            return false;
        }
        if (ModuleId(header.executableModId) != exportSignature)
        {
            // Time-stamp does not match executable.
            errorResult =
                "Module was exported from a different executable or the executable has changed";
            return false;
        }

        // Add to the loaded module table
        LoadedModuleData loadMod;
        loadMod.modId = header.thisModuleId;
        loadMod.fileName = fileName;
#ifndef _WIN32
        // Absolute, so that a state recording it can be loaded from another directory.
        {
            char *absolute = realpath(fileName.c_str(), NULL);
            if (absolute != NULL)
            {
                loadMod.fileName = absolute;
                free(absolute);
            }
        }
#endif
        loadMod.modDependencies.reserve(header.dependencyCount);

        // Get the dependencies.  This is only needed to fill in the dependency vector.
        if (fseek(loadFile, header.dependencies, SEEK_SET) != 0)
        {
            errorResult = "Unable to access dependency table";
            return false;
        }
        for (unsigned i = 0; i < header.dependencyCount; i++)
        {
            ModDependencyEntry modDepEntry;
            if (fread(&modDepEntry, sizeof(ModDependencyEntry), 1, loadFile) != 1)
            {
                errorResult = "Unable to load dependency entry";
                return false;
            }
            loadMod.modDependencies.push_back(modDepEntry.depId);
        }

        // We could check the dependency table at this point but other than perhaps improving
        // the error message if a dependency is missing there's no point.

        // Read all the descriptors first.  We need to be able to seek to the actual data for
        // each space as we create them.
        std::vector<ModuleSegmentDescr> descrs;
        descrs.reserve(header.segmentDescrCount);
        if (fseek(loadFile, header.segmentDescr, SEEK_SET) != 0)
        {
            errorResult = "Unable to read segment descriptors";
            return false;
        }
        for (unsigned i = 0; i < header.segmentDescrCount; i++)
        {
            ModuleSegmentDescr msd;
            if (fread(&msd, sizeof(ModuleSegmentDescr), 1, loadFile) != 1)
            {
                errorResult = "Unable to read segment descriptors";
                return false;
            }
            descrs.push_back(msd);
        }
        if (header.rootSegment >= header.segmentDescrCount ||
                header.rootOffset >= descrs[header.rootSegment].segmentSize)
        {
            errorResult = "Module root is outside its segment";
            return false;
        }
        // No space may have the module's id yet: a failed load removes the spaces with the
        // module's id, which must be only those it added.
        for (std::vector<PermanentMemSpace*>::iterator sp = gMem.pSpaces.begin(); sp < gMem.pSpaces.end(); sp++)
        {
            if ((*sp)->moduleIdentifier == ModuleId(header.thisModuleId))
            {
                errorResult = "Memory space with this signature already exists";
                return false;
            }
        }
        for (std::vector<ModuleSegmentDescr>::iterator descr = descrs.begin(); descr < descrs.end(); descr++)
        {
            if (descr->segmentData != 0 &&
                ModuleId(descr->moduleIden) != ModuleId(header.thisModuleId))
            {
                errorResult = "Memory space with this signature already exists";
                return false;
            }
        }

        std::vector<ModuleLoadData> loadData;
        loadData.reserve(header.segmentDescrCount);

#ifdef POLYML_MAP_SAVED_STATES
        // Only a segment wholly within the file is mapped: a page past the end of a truncated
        // file would raise SIGBUS when touched.  The read path below reports the short read.
        struct stat fileStat;
        uint64_t fileSize = fstat(fileno(loadFile), &fileStat) == 0 ? (uint64_t)fileStat.st_size : 0;
#endif
        PermanentMemSpace* mappedSpace = 0;
        std::vector<PermanentMemSpace*> volatileSpaces; // Read-in no-overwrite segments.
        // Once a segment has been added, a failure, an exception included, must remove the
        // module's segments again so that nothing is left half loaded or half relocated.
        // Only this load's segments have the module's id: that was checked above.
        struct AddedSegments {
            ModuleId modId;
            bool added;
            ~AddedSegments() { if (added) gMem.DeletePermanentSpaces(modId); }
        } addedSegments = { ModuleId(header.thisModuleId), false };
        auto fail = [&](const char* message) {
            errorResult = message;
            return false;
        };

        // Read in and create the new segments first.  If we have problems,
        // in particular if we have run out of memory, then it's easier to recover.
        for (std::vector<ModuleSegmentDescr>::iterator descr = descrs.begin(); descr < descrs.end(); descr++)
        {
            PermanentMemSpace* space = gMem.SpaceForIndex(descr->segmentIndex, descr->moduleIden);
            ModuleLoadData load;
            load.relocationCount = descr->relocationCount;
            load.relocRunCount = descr->relocRunCount;
            load.relocations = descr->relocations;

            if (descr->segmentData == 0)
            { // No data - just an entry in the index.
                if (space == NULL)
                {
                    return fail("Required memory space is not present");
                }
                if (descr->segmentSize != (size_t)((char*)space->topPointer - (char*)space->bottom))
                {
                    return fail("Required memory space has a different size");
                }
                // A module sets locations only in its own segments.
                if (descr->relocations != 0)
                {
                    return fail("Relocations in a segment the module does not own");
                }
                load.targetAddr = space->bottom;
                load.moved = (uintptr_t)space->bottom != descr->originalAddr;
            }
            else
            { // New segment.
                // Allocate memory for the new segment.
                unsigned mFlags =
                    (descr->segmentFlags & MSF_WRITABLE ? MTF_WRITEABLE : 0) |
                    (descr->segmentFlags & MSF_NOOVERWRITE ? MTF_NO_OVERWRITE : 0) |
                    (descr->segmentFlags & MSF_BYTES ? MTF_BYTES : 0) |
                    (descr->segmentFlags & MSF_CODE ? MTF_EXECUTABLE : 0);
                PermanentMemSpace* newSpace = 0;
#ifdef POLYML_MAP_SAVED_STATES
                // Map a prelinked segment where it was prelinked for.  If that address is
                // taken, read it in instead and apply all its relocations.  Relocations in it
                // to the module's other segments, which are read in, are applied below.
                if (descr->originalAddr != 0 && descr->segmentFlags == 0 &&
                        descr->segmentSize <= fileSize && (uint64_t)descr->segmentData <= fileSize - descr->segmentSize)
                {
                    newSpace = gMem.MapPermanentSpace((void*)descr->originalAddr, descr->segmentSize,
                        fileno(loadFile), descr->segmentData, mFlags, descr->segmentIndex, header.thisModuleId);
                    if (newSpace != 0)
                    {
                        addedSegments.added = true;
                        mappedSpace = newSpace;
                        load.moved = false;
                    }
                }
#endif
                if (newSpace == 0)
                {
                    // TODO: This creates the new space as though it existed in the original executable.
                    // We need to distinguish it somehow so we know which module it came from.
                    newSpace = gMem.AllocateNewPermanentSpace(descr->segmentSize, mFlags, descr->segmentIndex, header.thisModuleId);
                    if (newSpace == 0)
                    {
                        return fail("Unable to allocate memory");
                    }
                    addedSegments.added = true;
                    if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0)
                    {
                        return fail("Unable to seek to segment");
                    }
                    if (readData(newSpace->writeAble(newSpace->bottom), descr->segmentSize, loadFile) != 1)
                    {
                        return fail("Unable to read segment");
                    }
                    if (newSpace->noOverwrite)
                        volatileSpaces.push_back(newSpace);
                }
                load.targetAddr = newSpace->bottom;
            }
            loadData.push_back(load);
        }
        // Now deal with relocation.  A relocation sets a location to an address in a target
        // segment.  The data in the file already holds that value unless the segment it is
        // in or the target segment is not where the descriptors say.  When no segment
        // moved there is nothing to set, so the relocation tables are not read.
        bool anyMoved = false;
        for (std::vector<ModuleLoadData>::iterator i = loadData.begin(); i < loadData.end(); i++)
            anyMoved = anyMoved || i->moved;
        if (anyMoved)
            movedModuleLoads++;
        for (std::vector<ModuleLoadData>::iterator i = loadData.begin(); anyMoved && i < loadData.end(); i++)
        {
            PolyWord* baseAddr = i->targetAddr;
            size_t segmentSize = descrs[i - loadData.begin()].segmentSize;
            ASSERT(baseAddr != NULL); // We should have created it.
            // Process explicit relocations.
            if (i->relocations)
            {
                std::vector<ModRelocationRun> runs(i->relocRunCount);
                if (fseek(loadFile, i->relocations, SEEK_SET) != 0 ||
                        (!runs.empty() && fread(runs.data(), sizeof(ModRelocationRun), runs.size(), loadFile) != runs.size()))
                {
                    return fail("Unable to read relocation segment");
                }
                off_t runStart = i->relocations + (off_t)(runs.size() * sizeof(ModRelocationRun));
                std::vector<ModRelocationEntry> relocs;
                for (std::vector<ModRelocationRun>::iterator run = runs.begin(); run < runs.end(); run++)
                {
                    if (run->targetSegment >= header.segmentDescrCount)
                        return fail("Relocation to a segment the module does not have");
                    if (i->moved || loadData[run->targetSegment].moved)
                    {
                        relocs.resize(run->count);
                        if (fseek(loadFile, runStart, SEEK_SET) != 0 ||
                                fread(relocs.data(), sizeof(ModRelocationEntry), run->count, loadFile) != run->count)
                            return fail("Unable to read relocation segment");
                        byte* targetBase = (byte*)loadData[run->targetSegment].targetAddr;
                        size_t targetSize = descrs[run->targetSegment].segmentSize;
                        for (std::vector<ModRelocationEntry>::iterator reloc = relocs.begin(); reloc < relocs.end(); reloc++)
                        {
                            if (reloc->relKind > PROCESS_RELOC_C32ADDR || reloc->relocAddress > segmentSize ||
                                    RelocationSize(reloc->relKind) > segmentSize - reloc->relocAddress ||
                                    reloc->targetAddress > targetSize)
                                return fail("Relocation outside its segment");
                            byte* setAddress = (byte*)baseAddr + reloc->relocAddress;
                            ScanAddress::SetConstantValue(setAddress, (PolyObject*)(targetBase + reloc->targetAddress), reloc->relKind);
                        }
                    }
                    runStart += (off_t)(run->count * sizeof(ModRelocationEntry));
                }
            }
        }
        // Set external references and clear other weak byte refs, as loading a state does.
        // After relocation, which would otherwise set the pointers it clears again when a
        // segment moved.
        for (std::vector<PermanentMemSpace*>::iterator v = volatileSpaces.begin(); v < volatileSpaces.end(); v++)
        {
            ClearVolatile cwbr;
            cwbr.ScanAddressesInRegion((*v)->bottom, (*v)->topPointer);
        }
#ifdef POLYML_MAP_SAVED_STATES
        if (mappedSpace != 0)
            gMem.WriteProtectMappedSpace(mappedSpace);
#endif

        // Get the root address.
        {
            PolyWord* baseAddr = loadData[header.rootSegment].targetAddr;
            root = (PolyObject*)((byte*)baseAddr + header.rootOffset);
        }
        loadedId = header.thisModuleId;

        // Now add the entry to the loaded module table
        loadedModules.push_back(loadMod);
        try {
            loadedModuleIds.insert(ModuleKey(header.thisModuleId));
        }
        catch (...) {
            loadedModules.pop_back();
            throw;
        }
        addedSegments.added = false;
    }
    return errorResult == NULL;
}

void ModuleLoader::Perform()
{
    try {
        PolyObject* root;
        if (loadModuleFile(fileName, NULL, errorResult, errNumber, root, loadedModId))
        {
            // Push the root to the caller's save vec.  If we put the
            // newly created areas into local memory we could get a GC as soon as we
            // complete this root request.
            rootHandle = callerTaskData->saveVec.push(root);
        }
    }
    catch (const std::bad_alloc&)
    {
        errorResult = "Insufficient Memory : C++ allocation failed";
    }
    catch (const std::length_error&) // A table size from the file is too large.
    {
        errorResult = "Insufficient Memory : C++ allocation failed";
    }
}

static Handle LoadModule(TaskData* taskData, Handle args)
{
    std_tstring fileName = PolyStringToTString(args->Word());
    ModuleLoader loader(taskData, fileName);
    processes->MakeRootRequest(taskData, &loader);

    if (loader.errorResult != 0)
    {
        if (loader.errNumber == 0)
            raise_fail(taskData, loader.errorResult);
        else
        {
            AutoFree<char*> buff((char*)malloc(strlen(loader.errorResult) + 2 + loader.fileName.size() * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
            sprintf(buff, "%s: %S", loader.errorResult, loader.fileName.c_str());
#else
            sprintf(buff, "%s: %s", loader.errorResult, loader.fileName.c_str());
#endif
            raise_syscall(taskData, buff, loader.errNumber);
        }
    }

    // Allocate the module id before the pair: an allocation can GC and move
    // the pair while a raw pointer to it is held.
    Handle modId = moduleIdAsByteVector(taskData, loader.loadedModId);
    Handle result = alloc_and_save(taskData, 2);
    result->WordP()->Set(0, loader.rootHandle->Word());
    result->WordP()->Set(1, modId->Word());

    return result;
}

// Load a module
POLYUNSIGNED PolyLoadModule(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = LoadModule(taskData, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

// Return the system directory for modules.  This is configured differently
// in Unix and in Windows.
POLYUNSIGNED PolyGetModuleDirectory(POLYUNSIGNED threadId)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle result = 0;

    try {
#if (defined(MODULEDIR))
        result = SAVE(C_string_to_Poly(taskData, MODULEDIR));
#elif (defined(_WIN32))
        {
            // This registry key is configured when Poly/ML is installed using the installer.
            // It gives the path to the Poly/ML installation directory.  We return the
            // Modules subdirectory.
            HKEY hk;
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\PolyML.exe"), 0,
                KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS)
            {
                DWORD valSize;
                if (RegQueryValueEx(hk, _T("Path"), 0, NULL, NULL, &valSize) == ERROR_SUCCESS)
                {
#define MODULEDIR _T("Modules")
                    TempString buff((TCHAR*)malloc(valSize + (_tcslen(MODULEDIR) + 1) * sizeof(TCHAR)));
                    DWORD dwType;
                    if (RegQueryValueEx(hk, _T("Path"), 0, &dwType, (LPBYTE)(LPTSTR)buff, &valSize) == ERROR_SUCCESS)
                    {
                        // The registry entry should end with a backslash.
                        _tcscat(buff, MODULEDIR);
                        result = SAVE(C_string_to_Poly(taskData, buff));
                    }
                }
                RegCloseKey(hk);
            }
            result = SAVE(C_string_to_Poly(taskData, ""));
        }
#else
        result = SAVE(C_string_to_Poly(taskData, ""));
#endif
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}


// Return the list of loaded modules as a list of pairs of name and signature.
POLYUNSIGNED PolyShowLoadedModules(POLYUNSIGNED threadId)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle result = taskData->saveVec.push(ListNull); // Head of list

    try {
        // Iterate over the table in reverse order so  the resulting list is in the order the modules were loaded.
        for (std::vector<LoadedModuleData>::reverse_iterator i = loadedModules.rbegin(); i < loadedModules.rend(); i++)
        {
            // Convert the module Id to a vector of bytes.  The representation is the same as a string.
            Handle idHandle = moduleIdAsByteVector(taskData, i->modId);
            ML_Cons_Cell* next = (ML_Cons_Cell*)alloc(taskData, sizeof(ML_Cons_Cell) / sizeof(PolyWord));
            next->h = idHandle->Word();
            next->t = result->Word();
            // Reset the save vec to keep it bounded
            taskData->saveVec.reset(reset);
            result = taskData->saveVec.push(next);
        }
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

static Handle GetModInfo(TaskData* taskData, Handle hFileName)
{
    std_tstring fileName = PolyStringToTString(hFileName->Word());
    AutoClose loadFile(_tfopen(fileName.c_str(), _T("rb")));
    if ((FILE*)loadFile == NULL)
        raise_syscall(taskData, "Cannot open file", ERRORNUMBER); // The string will be set from the error code
    ModuleHeader header;

    // Read the header and check the signature.
    if (fread(&header, sizeof(ModuleHeader), 1, loadFile) != 1)
        raise_fail(taskData, "Unable to load header");
    if (strncmp(header.headerSignature, MODULESIGNATURE, sizeof(header.headerSignature)) != 0)
        raise_fail(taskData, "File is not a Poly/ML module");
    if (header.headerVersion != MODULEVERSION ||
        header.headerLength != sizeof(ModuleHeader) ||
        header.segmentDescrLength != sizeof(ModuleSegmentDescr))
        raise_fail(taskData, "Unsupported version of module file");
    // Check that the module came from this executable.  We could include the
    // signature of the executable in the result but then we would also have to
    // have some way to provide the current value of exportSignature in order to check it.
    if (ModuleId(header.executableModId) != exportSignature)
        // Time-stamp does not match executable.
        raise_fail(taskData, "Module was exported from a different executable or the executable has changed");

    if (header.stringTableSize == 0)
        raise_fail(taskData, "Missing string table");
    // String table.  All sizes and offsets are in bytes but the contents are wide chars if
    // Unicode is being used.
    AutoFree<char*> stringTab = (char*)malloc(header.stringTableSize);
    if (stringTab == 0)
        raise_fail(taskData, "Unable to allocate memory");
    if (fseek(loadFile, header.stringTable, SEEK_SET) != 0 ||
        fread(stringTab, 1, header.stringTableSize, loadFile) != header.stringTableSize)
        raise_fail(taskData, "Unable to read string table");
    // Check that there is at least one terminator
    if (stringTab[header.stringTableSize - 1] != 0)
        raise_fail(taskData, "Malformed string table");

    Handle reset = taskData->saveVec.mark();
    Handle depListHandle = taskData->saveVec.push(ListNull);
    if (fseek(loadFile, header.dependencies, SEEK_SET) != 0)
        raise_fail(taskData, "Unable to access dependency table");

    for (unsigned i = 0; i < header.dependencyCount; i++)
    {
        ModDependencyEntry modDepEntry;
        if (fread(&modDepEntry, sizeof(ModDependencyEntry), 1, loadFile) != 1)
            raise_fail(taskData, "Unable to load dependency entry");
        // Convert the module Id to a vector of bytes.  The representation is the same as a string.
        Handle idHandle = moduleIdAsByteVector(taskData, modDepEntry.depId);
        if (modDepEntry.depNameEntry >= header.stringTableSize)
            raise_fail(taskData, "Invalid string table entry");
        const TCHAR* thisString = (TCHAR*)(stringTab + modDepEntry.depNameEntry);
        Handle nameHandle = taskData->saveVec.push(C_string_to_Poly(taskData, thisString));
        Handle pairHandle = alloc_and_save(taskData, 2);
        pairHandle->WordP()->Set(0, idHandle->Word());
        pairHandle->WordP()->Set(1, nameHandle->Word());
        ML_Cons_Cell* next = (ML_Cons_Cell*)alloc(taskData, sizeof(ML_Cons_Cell) / sizeof(PolyWord));
        next->h = pairHandle->Word();
        next->t = depListHandle->Word();
        // Reset the save vec to keep it bounded
        taskData->saveVec.reset(reset);
        depListHandle = taskData->saveVec.push(next);
    }

    Handle modIdHandle = moduleIdAsByteVector(taskData, header.thisModuleId);

    Handle resultHandle = alloc_and_save(taskData, 2);
    resultHandle->WordP()->Set(0, modIdHandle->Word());
    resultHandle->WordP()->Set(1, depListHandle->Word());

    return resultHandle;
}

// Open a module file and read the module information and dependencies
POLYUNSIGNED PolyGetModuleInfo(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = GetModInfo(taskData, pushedArg);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

class ModuleReleaser : public MainThreadRequest
{
public:
    ModuleReleaser(ModuleId mId) : MainThreadRequest(MTP_RELEASEMODULE), moduleId(mId), errorMessage(0) {
    }
    virtual void Perform() override;

    ModuleId moduleId;
    const char* errorMessage;

};

void ModuleReleaser::Perform()
{
    try {
        std::vector<LoadedModuleData>::iterator mod = loadedModules.begin();
        while (true)
        {
            if (mod == loadedModules.end())
            {
                errorMessage = "Module is not loaded";
                return;
            }
            if (mod->modId == moduleId)
                break;
            else mod++;
        }
        // A loaded saved state may refer to the module's data.
        for (std::vector<HierarchyTable>::const_iterator h = hierarchyTable.cbegin(); h != hierarchyTable.cend(); h++)
        {
            for (std::vector<ModuleId>::const_iterator m = h->modules.cbegin(); m != h->modules.cend(); m++)
            {
                if (*m == moduleId)
                {
                    errorMessage = "Module is used by a loaded saved state";
                    return;
                }
            }
        }
        // The module stays registered until all its spaces are demoted, so a
        // failed release can be retried.
        if (!gMem.DemoteOldPermanentSpaces(moduleId))
        {
            errorMessage = "Insufficient Memory";
            return;
        }
        loadedModules.erase(mod);
        loadedModuleIds.erase(ModuleKey(moduleId));
    }
    catch (const std::bad_alloc&)
    {
        errorMessage = "Insufficient Memory : C++ allocation failed";
    }
}

POLYUNSIGNED PolyReleaseModule(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData* taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();

    try {
        ModuleId modId = moduleIdFromByteVector(taskData, PolyWord::FromUnsigned(arg));
        ModuleReleaser modRelease(modId);
        processes->MakeRootRequest(taskData, &modRelease);
        if (modRelease.errorMessage != 0)
            raise_fail(taskData, modRelease.errorMessage);
    }
    catch (const std::bad_alloc&) { setMemoryException(taskData); }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

struct _entrypts savestateEPT[] =
{
    { "PolySaveState",                  (polyRTSFunction)&PolySaveState },
    { "PolySaveStateWithModules",       (polyRTSFunction)&PolySaveStateWithModules },
    { "PolyShowModuleDependencies",     (polyRTSFunction)&PolyShowModuleDependencies },
    { "PolyLoadState",                  (polyRTSFunction)&PolyLoadState },
    { "PolyGetHierarchy",               (polyRTSFunction)&PolyGetHierarchy },
    { "PolyRenameParent",               (polyRTSFunction)&PolyRenameParent },
    { "PolyShowParent",                 (polyRTSFunction)&PolyShowParent },
    { "PolyLoadHierarchy",              (polyRTSFunction)&PolyLoadHierarchy },
    { "PolyStoreModule",                (polyRTSFunction)&PolyStoreModule },
    { "PolyLoadModule",                 (polyRTSFunction)&PolyLoadModule },
    { "PolyGetModuleDirectory",         (polyRTSFunction)&PolyGetModuleDirectory },
    { "PolyShowLoadedModules",          (polyRTSFunction)&PolyShowLoadedModules },
    { "PolyGetModuleInfo",              (polyRTSFunction)&PolyGetModuleInfo },
    { "PolyReleaseModule",              (polyRTSFunction)&PolyReleaseModule },

    { NULL, NULL } // End of list.
};

