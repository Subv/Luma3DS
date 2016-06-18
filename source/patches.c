/*
*   patches.c
*/

#include "patches.h"
#include "memory.h"
#include "cache.h"
#include "config.h"
#include "fs.h"
#include "../build/rebootpatch.h"
#include "../build/smpatch.h"
#include "tinyprintf/tinyprintf.h"

u8 *getProcess9(u8 *pos, u32 size, u32 *process9Size, u32 *process9MemAddr)
{
    u8 *off = memsearch(pos, "ess9", size, 4);

    *process9Size = *(u32 *)(off - 0x60) * 0x200;
    *process9MemAddr = *(u32 *)(off + 0xC);

    //Process9 code offset (start of NCCH + ExeFS offset + ExeFS header size)
    return off - 0x204 + (*(u32 *)(off - 0x64) * 0x200) + 0x200;
}

void patchSignatureChecks(u8 *pos, u32 size)
{
    const u16 sigPatch[2] = {0x2000, 0x4770};

    //Look for signature checks
    const u8 pattern[] = {0xC0, 0x1C, 0x76, 0xE7},
             pattern2[] = {0xB5, 0x22, 0x4D, 0x0C};

    u16 *off = (u16 *)memsearch(pos, pattern, size, 4),
        *off2 = (u16 *)(memsearch(pos, pattern2, size, 4) - 1);

    *off = sigPatch[0];
    off2[0] = sigPatch[0];
    off2[1] = sigPatch[1];
}

void patchFirmlaunches(u8 *pos, u32 size, u32 process9MemAddr)
{
    //Look for firmlaunch code
    const u8 pattern[] = {0xDE, 0x1F, 0x8D, 0xE2};

    u8 *off = memsearch(pos, pattern, size, 4) - 0x10;

    //Firmlaunch function offset - offset in BLX opcode (A4-16 - ARM DDI 0100E) + 1
    u32 fOpenOffset = (u32)(off + 9 - (-((*(u32 *)off & 0x00FFFFFF) << 2) & (0xFFFFFF << 2)) - pos + process9MemAddr);

    //Copy firmlaunch code
    memcpy(off, reboot, reboot_size);

    //Put the fOpen offset in the right location
    u32 *pos_fopen = (u32 *)memsearch(off, "OPEN", reboot_size, 4);
    *pos_fopen = fOpenOffset;
}

void patchFirmWrites(u8 *pos, u32 size)
{
    const u16 writeBlock[2] = {0x2000, 0x46C0};

    //Look for FIRM writing code
    u8 *const off1 = memsearch(pos, "exe:", size, 4);
    const u8 pattern[] = {0x00, 0x28, 0x01, 0xDA};

    u16 *off2 = (u16 *)memsearch(off1 - 0x100, pattern, 0x100, 4);

    off2[0] = writeBlock[0];
    off2[1] = writeBlock[1];
}

void patchFirmWriteSafe(u8 *pos, u32 size)
{
    const u16 writeBlockSafe[2] = {0x2400, 0xE01D};

    //Look for FIRM writing code
    const u8 pattern[] = {0x04, 0x1E, 0x1D, 0xDB};

    u16 *off = (u16 *)memsearch(pos, pattern, size, 4);

    off[0] = writeBlockSafe[0];
    off[1] = writeBlockSafe[1];
}

void reimplementSvcBackdoor(u8 *pos, u32 size)
{
    //Official implementation of svcBackdoor
    const u8  svcBackdoor[40] = {0xFF, 0x10, 0xCD, 0xE3,  //bic   r1, sp, #0xff
                                 0x0F, 0x1C, 0x81, 0xE3,  //orr   r1, r1, #0xf00
                                 0x28, 0x10, 0x81, 0xE2,  //add   r1, r1, #0x28
                                 0x00, 0x20, 0x91, 0xE5,  //ldr   r2, [r1]
                                 0x00, 0x60, 0x22, 0xE9,  //stmdb r2!, {sp, lr}
                                 0x02, 0xD0, 0xA0, 0xE1,  //mov   sp, r2
                                 0x30, 0xFF, 0x2F, 0xE1,  //blx   r0
                                 0x03, 0x00, 0xBD, 0xE8,  //pop   {r0, r1}
                                 0x00, 0xD0, 0xA0, 0xE1,  //mov   sp, r0
                                 0x11, 0xFF, 0x2F, 0xE1}; //bx    r1

    const u8 pattern[] = {0x00, 0xB0, 0x9C, 0xE5}; //cpsid aif
    
    u32 *exceptionsPage = (u32 *)memsearch(pos, pattern, size, 4) - 0xB;

    u32 svcOffset = (-((exceptionsPage[2] & 0xFFFFFF) << 2) & (0xFFFFFF << 2)) - 8; //Branch offset + 8 for prefetch
    u32 *svcTable = (u32 *)(pos + *(u32 *)(pos + 0xFFFF0008 - svcOffset - 0xFFF00000 + 8) - 0xFFF00000); //SVC handler address
    while(*svcTable) svcTable++; //Look for SVC0 (NULL)

    if(!svcTable[0x7B])
    {
        u32 *freeSpace;
        for(freeSpace = exceptionsPage; *freeSpace != 0xFFFFFFFF; freeSpace++);

        memcpy(freeSpace, svcBackdoor, 40);

        svcTable[0x7B] = 0xFFFF0000 + ((u8 *)freeSpace - (u8 *)exceptionsPage);
    }
}

void patchServiceAccessCheck(u8 *pos, u32 size)
{
    // Create a codecave in the empty padding space after the kernel
    // This codecave will patch the sm module after it is decompressed

    // Find some padding space to add our code
    const u8 bogus_pattern[] = {0x1E, 0xFF, 0x2F, 0xE1, 0x1E, 0xFF, 0x2F, 0xE1, 0x1E, 0xFF, 
  0x2F, 0xE1, 0x00, 0x10, 0xA0, 0xE3, 0x00, 0x10, 0xC0, 0xE5, 
  0x1E, 0xFF, 0x2F, 0xE1};
    
    u32 *someSpace = (u32 *)memsearch(pos, bogus_pattern, size, 24);

    u32 *freeSpace;
    for(freeSpace = someSpace; *freeSpace != 0xFFFFFFFF; freeSpace++);

    if (freeSpace != NULL) {
        // Inject the codecave
        memcpy(freeSpace, sm, sm_size);
    } else {
        if (freeSpace != NULL)
            fileWrite("NotFoundP", "patch.log", 9);
        else
            fileWrite("NotFoundS", "patch.log", 9);
        return;
    }

    // Patch the built-in module loading code to jump to our codecave
    // Find the code that decompresses the .code section
    const u8 pattern[] = {0x00, 0x00, 0x94, 0xE5, 0x18, 0x10, 0x90, 0xE5, 0x28, 0x20, 
                          0x90, 0xE5, 0x48, 0x00, 0x9D, 0xE5};

    u8 *off = memsearch(pos, pattern, size, 16);

    if (off != NULL) {
        char buff[100];
        tfp_snprintf(buff, 100, "%08X - %08X", (u32)freeSpace, (u32)off);
        fileWrite(buff, "patch.log", 19);

        tfp_snprintf(buff, 100, "%08X", (u32)pos);
        fileWrite(buff, "base.log", 8);

        // Inject a jump instruction to our codecave at off
        // Construct a jump instruction to our codecave
        u32 offset = ((((u32)freeSpace) - ((u32)off + 8)) >> 2) & 0xFFFFFF;
        u32 instruction = offset | (1 << 24) | (0x5 << 25) | (0xE << 28);

        u32* to_patch = (u32*)off;
        u32 previous = *to_patch;
        *to_patch = instruction;

        tfp_snprintf(buff, 100, "%08X - %08X - %08X - %08X", (u32)offset, (u32)instruction, previous, *to_patch);
        fileWrite(buff, "instr.log", 41);

        // Clear the data cache
        flushEntireDCache();
        flushEntireICache();

        fileWrite(pos, "firmware_11x4.bin", size);
    } else {
        fileWrite("NotFound", "patch.log", 8);
        return;
    }
}

void patchTitleInstallMinVersionCheck(u8 *pos, u32 size)
{
    const u8 pattern[] = {0x0A, 0x81, 0x42, 0x02};
    
    u8 *off = memsearch(pos, pattern, size, 4);
    
    if(off != NULL) off[4] = 0xE0;
}

void applyLegacyFirmPatches(u8 *pos, FirmwareType firmType, u32 isN3DS)
{
    const patchData twlPatches[] = {
        {{0x1650C0, 0x165D64}, {{ 6, 0x00, 0x20, 0x4E, 0xB0, 0x70, 0xBD }}, 0},
        {{0x173A0E, 0x17474A}, { .type1 = 0x2001 }, 1},
        {{0x174802, 0x17553E}, { .type1 = 0x2000 }, 2},
        {{0x174964, 0x1756A0}, { .type1 = 0x2000 }, 2},
        {{0x174D52, 0x175A8E}, { .type1 = 0x2001 }, 2},
        {{0x174D5E, 0x175A9A}, { .type1 = 0x2001 }, 2},
        {{0x174D6A, 0x175AA6}, { .type1 = 0x2001 }, 2},
        {{0x174E56, 0x175B92}, { .type1 = 0x2001 }, 1},
        {{0x174E58, 0x175B94}, { .type1 = 0x4770 }, 1}
    },
    agbPatches[] = {
        {{0x9D2A8, 0x9DF64}, {{ 6, 0x00, 0x20, 0x4E, 0xB0, 0x70, 0xBD }}, 0},
        {{0xD7A12, 0xD8B8A}, { .type1 = 0xEF26 }, 1}
    };

    /* Calculate the amount of patches to apply. Only count the boot screen patch for AGB_FIRM
       if the matching option was enabled (keep it as last) */
    u32 numPatches = firmType == TWL_FIRM ? (sizeof(twlPatches) / sizeof(patchData)) :
                                            (sizeof(agbPatches) / sizeof(patchData) - !CONFIG(6));
    const patchData *patches = firmType == TWL_FIRM ? twlPatches : agbPatches;

    //Patch
    for(u32 i = 0; i < numPatches; i++)
    {
        switch(patches[i].type)
        {
            case 0:
                memcpy(pos + patches[i].offset[isN3DS], patches[i].patch.type0 + 1, patches[i].patch.type0[0]);
                break;
            case 2:
                *(u16 *)(pos + patches[i].offset[isN3DS] + 2) = 0;
            case 1:
                *(u16 *)(pos + patches[i].offset[isN3DS]) = patches[i].patch.type1;
                break;
        }
    }
}

u32 getLoader(u8 *pos, u32 *loaderSize)
{
    u8 *off = pos;
    u32 size;

    while(1)
    {
        size = *(u32 *)(off + 0x104) * 0x200;
        if(*(u32 *)(off + 0x200) == 0x64616F6C) break;
        off += size;
    }

    *loaderSize = size;

    return (u32)(off - pos);
}