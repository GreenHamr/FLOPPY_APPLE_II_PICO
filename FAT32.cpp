#include "FAT32.h"
#include "SDCardManager.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Constructor
FAT32::FAT32(SDCardManager* sdCardManager) {
    sdCard = sdCardManager;
    partitionStartSector = 0;
    memset(&bootSector, 0, sizeof(bootSector));
    fatStartSector = 0;
    dataStartSector = 0;
    rootDirStartSector = 0;
    bytesPerCluster = 0;
    sectorsPerCluster = 0;
    currentDirCluster = 0;
    memset(currentPath, 0, sizeof(currentPath));
    strcpy(currentPath, "/");
    lastError = FAT32_OK;
}

// Detect filesystem type from boot sector
// Returns: 0 = unknown, 1 = FAT12, 2 = FAT16, 3 = FAT32, 4 = exFAT, 5 = NTFS
static int detectFilesystemType(uint8_t* bootSectorBuffer) {
    // Check for exFAT signature at offset 3
    if (memcmp(bootSectorBuffer + 3, "EXFAT   ", 8) == 0) {
        return 4;  // exFAT
    }
    
    // Check for NTFS signature at offset 3
    if (memcmp(bootSectorBuffer + 3, "NTFS    ", 8) == 0) {
        return 5;  // NTFS
    }
    
    // Check for valid boot sector signature (0x55AA at offset 510)
    if (bootSectorBuffer[510] != 0x55 || bootSectorBuffer[511] != 0xAA) {
        return 0;  // Unknown or unformatted
    }
    
    // Check bytes per sector (offset 11-12, little endian)
    uint16_t bytesPerSector = bootSectorBuffer[11] | (bootSectorBuffer[12] << 8);
    if (bytesPerSector != 512 && bytesPerSector != 1024 && 
        bytesPerSector != 2048 && bytesPerSector != 4096) {
        return 0;  // Invalid sector size
    }
    
    // Check for FAT32 signature at offset 82
    if (memcmp(bootSectorBuffer + 82, "FAT32   ", 8) == 0) {
        return 3;  // FAT32
    }
    
    // Check for FAT16 signature at offset 54
    if (memcmp(bootSectorBuffer + 54, "FAT16   ", 8) == 0) {
        return 2;  // FAT16
    }
    
    // Check for FAT12 signature at offset 54
    if (memcmp(bootSectorBuffer + 54, "FAT12   ", 8) == 0) {
        return 1;  // FAT12
    }
    
    // Check for FAT signature at offset 54 (generic)
    if (memcmp(bootSectorBuffer + 54, "FAT     ", 8) == 0) {
        // Determine FAT type by checking sectors_per_fat_32 (offset 36-39)
        uint32_t sectorsPerFat32 = bootSectorBuffer[36] | (bootSectorBuffer[37] << 8) |
                                   (bootSectorBuffer[38] << 16) | (bootSectorBuffer[39] << 24);
        if (sectorsPerFat32 != 0) {
            return 3;  // FAT32 (has 32-bit sectors per FAT)
        }
        return 2;  // Assume FAT16
    }
    
    // Check sectors_per_fat_32 field (offset 36-39) for FAT32 without signature
    uint32_t sectorsPerFat32 = bootSectorBuffer[36] | (bootSectorBuffer[37] << 8) |
                               (bootSectorBuffer[38] << 16) | (bootSectorBuffer[39] << 24);
    uint16_t sectorsPerFat16 = bootSectorBuffer[22] | (bootSectorBuffer[23] << 8);
    
    if (sectorsPerFat32 != 0 && sectorsPerFat16 == 0) {
        return 3;  // FAT32
    }
    
    if (sectorsPerFat16 != 0) {
        return 2;  // FAT16 or FAT12
    }
    
    return 0;  // Unknown
}

// Parse MBR partition table and find first partition
// Returns the start sector of the first partition, or 0 if no partition table
static uint32_t findFirstPartition(uint8_t* mbrBuffer) {
    // Check for valid MBR signature (0x55AA at offset 510)
    if (mbrBuffer[510] != 0x55 || mbrBuffer[511] != 0xAA) {
        return 0;  // No valid MBR
    }
    
    // Check if this is an MBR (partition table at offset 446)
    // or a VBR (Volume Boot Record - actual filesystem)
    // MBR typically has 0x00 at offset 0, VBR has jump instruction (0xEB or 0xE9)
    
    // Check for filesystem signatures that indicate this is a VBR, not MBR
    if (memcmp(mbrBuffer + 3, "MSDOS", 5) == 0 ||
        memcmp(mbrBuffer + 3, "MSWIN", 5) == 0 ||
        memcmp(mbrBuffer + 3, "mkdosfs", 7) == 0 ||
        memcmp(mbrBuffer + 3, "EXFAT", 5) == 0 ||
        memcmp(mbrBuffer + 3, "NTFS", 4) == 0 ||
        memcmp(mbrBuffer + 54, "FAT", 3) == 0 ||
        memcmp(mbrBuffer + 82, "FAT32", 5) == 0) {
        return 0;  // This is a VBR (boot sector), not MBR
    }
    
    // Parse partition table (4 entries starting at offset 446)
    for (int i = 0; i < 4; i++) {
        uint8_t* entry = mbrBuffer + 446 + (i * 16);
        
        // Partition type at offset 4
        uint8_t partType = entry[4];
        
        // Skip empty partitions
        if (partType == 0x00) {
            continue;
        }
        
        // Start LBA at offset 8 (little endian)
        uint32_t startLBA = entry[8] | (entry[9] << 8) | 
                           (entry[10] << 16) | (entry[11] << 24);
        
        // Accept FAT32, FAT16, exFAT, NTFS partition types
        // 0x01 = FAT12
        // 0x04 = FAT16 <32MB
        // 0x06 = FAT16 >32MB
        // 0x07 = NTFS/exFAT/HPFS
        // 0x0B = FAT32 (CHS)
        // 0x0C = FAT32 (LBA)
        // 0x0E = FAT16 (LBA)
        // 0x0F = Extended (LBA)
        if (partType == 0x01 || partType == 0x04 || partType == 0x06 ||
            partType == 0x07 || partType == 0x0B || partType == 0x0C ||
            partType == 0x0E) {
            return startLBA;
        }
    }
    
    return 0;  // No suitable partition found
}

// Initialize FAT32 filesystem
bool FAT32::init() {
    lastError = FAT32_OK;
    
    if (!sdCard) {
        lastError = FAT32_ERROR_NO_SDCARD;
        return false;
    }
    
    // Read sector 0 (could be MBR or VBR)
    uint8_t sectorBuffer[512];
    memset(sectorBuffer, 0, 512);
    
    // Try multiple times to read sector 0
    bool readSuccess = false;
    for (int retry = 0; retry < 5; retry++) {
        if (sdCard->readBlock(0, sectorBuffer)) {
            readSuccess = true;
            break;
        }
        sleep_ms(100);
    }
    
    if (!readSuccess) {
        lastError = FAT32_ERROR_READ_FAILED;
        return false;
    }
    
    // Check if this is an MBR with partition table
    uint32_t partitionStart = findFirstPartition(sectorBuffer);
    uint32_t bootSectorLBA = 0;
    uint8_t bootSectorBuffer[512];
    
    if (partitionStart > 0) {
        // This is an MBR, read the actual boot sector from partition
        bootSectorLBA = partitionStart;
        
        if (!sdCard->readBlock(partitionStart, bootSectorBuffer)) {
            lastError = FAT32_ERROR_READ_FAILED;
            return false;
        }
    } else {
        // No partition table, sector 0 is the boot sector (superfloppy format)
        bootSectorLBA = 0;
        memcpy(bootSectorBuffer, sectorBuffer, 512);
    }
    
    // Detect filesystem type
    int fsType = detectFilesystemType(bootSectorBuffer);
    
    // Handle non-FAT32 filesystems
    if (fsType == 4) {  // exFAT
        lastError = FAT32_ERROR_EXFAT;
        return false;
    }
    
    if (fsType == 5) {  // NTFS
        lastError = FAT32_ERROR_NTFS;
        return false;
    }
    
    if (fsType == 1) {  // FAT12
        lastError = FAT32_ERROR_FAT12;
        return false;
    }
    
    if (fsType == 2) {  // FAT16
        lastError = FAT32_ERROR_FAT16;
        return false;
    }
    
    if (fsType == 0) {  // Unknown
        lastError = FAT32_ERROR_UNKNOWN_FS;
        return false;
    }
    
    // At this point, fsType should be 3 (FAT32)
    memcpy(&bootSector, bootSectorBuffer, sizeof(FAT32_BootSector));
    
    // Store partition start sector for all subsequent reads
    partitionStartSector = bootSectorLBA;
    
    // Verify FAT32 parameters
    if (bootSector.bytes_per_sector != 512) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }
    
    if (bootSector.sectors_per_cluster == 0) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }
    
    // Calculate FAT32 structure (all sectors are relative to partition start)
    sectorsPerCluster = bootSector.sectors_per_cluster;
    bytesPerCluster = bootSector.bytes_per_sector * sectorsPerCluster;
    
    // FAT start sector (relative to partition start)
    fatStartSector = bootSector.reserved_sectors;
    
    // Data start sector = reserved + (num_fats * sectors_per_fat)
    dataStartSector = bootSector.reserved_sectors + 
                     (bootSector.num_fats * bootSector.sectors_per_fat_32);
    
    // Root directory starts at root_cluster
    rootDirStartSector = getClusterSector(bootSector.root_cluster);
    
    // Initialize current directory to root
    currentDirCluster = bootSector.root_cluster;
    strcpy(currentPath, "/");
    
    return true;
}

// Get sector number for a cluster (includes partition offset)
uint32_t FAT32::getClusterSector(uint32_t cluster) {
    // First data cluster is 2, so subtract 2
    // Add partition start sector for absolute addressing
    return partitionStartSector + dataStartSector + ((cluster - 2) * sectorsPerCluster);
}

// Read FAT entry for a cluster
uint32_t FAT32::readFATEntry(uint32_t cluster) {
    if (!sdCard) {
        return FAT32_CLUSTER_EOF_MIN;
    }
    
    // FAT32 uses 4 bytes per entry
    uint32_t fatOffset = cluster * 4;
    // Add partition start sector for absolute addressing
    uint32_t fatSector = partitionStartSector + fatStartSector + (fatOffset / 512);
    uint32_t fatEntryOffset = fatOffset % 512;
    
    uint8_t fatBuffer[512];
    if (!sdCard->readBlock(fatSector, fatBuffer)) {
        return FAT32_CLUSTER_EOF_MIN;
    }
    
    // Read 32-bit FAT entry (little endian)
    uint32_t nextCluster = fatBuffer[fatEntryOffset] |
                           (fatBuffer[fatEntryOffset + 1] << 8) |
                           (fatBuffer[fatEntryOffset + 2] << 16) |
                           (fatBuffer[fatEntryOffset + 3] << 24);
    
    // Mask to 28 bits (FAT32 uses 28 bits)
    return nextCluster & 0x0FFFFFFF;
}

// Read a cluster
bool FAT32::readCluster(uint32_t cluster, uint8_t* buffer) {
    if (!sdCard || !buffer) {
        return false;
    }
    
    uint32_t startSector = getClusterSector(cluster);
    
    // Read all sectors in the cluster
    for (uint32_t i = 0; i < sectorsPerCluster; i++) {
        if (!sdCard->readBlock(startSector + i, buffer + (i * 512))) {
            return false;
        }
    }
    
    return true;
}

// Format filename to 8.3 format
void FAT32::format83Name(const char* filename, char* name83) {
    memset(name83, ' ', 11);
    
    const char* dot = strchr(filename, '.');
    const char* nameStart = filename;
    
    // Skip leading path separators
    while (*nameStart == '/' || *nameStart == '\\') {
        nameStart++;
    }
    
    // Extract base name (up to 8 characters)
    int nameLen = 0;
    if (dot) {
        nameLen = dot - nameStart;
    } else {
        nameLen = strlen(nameStart);
    }
    
    if (nameLen > 8) nameLen = 8;
    
    for (int i = 0; i < nameLen; i++) {
        name83[i] = toupper(nameStart[i]);
    }
    
    // Extract extension (3 characters)
    if (dot) {
        const char* ext = dot + 1;
        int extLen = strlen(ext);
        if (extLen > 3) extLen = 3;
        
        for (int i = 0; i < extLen; i++) {
            name83[8 + i] = toupper(ext[i]);
        }
    }
}

// Compare 8.3 name with filename
bool FAT32::compare83Name(const char* name83, const char* filename) {
    char formatted83[11];
    format83Name(filename, formatted83);
    return memcmp(name83, formatted83, 11) == 0;
}

// Calculate LFN checksum from 8.3 name
uint8_t FAT32::calculateLFNChecksum(const uint8_t* shortName) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortName[i];
    }
    return sum;
}

// Read LFN entries before a directory entry
// Returns true if LFN was found and decoded
bool FAT32::readLFNEntries(FAT32_DirEntry* entries, int count, char* lfnBuffer, uint32_t bufferSize) {
    if (!entries || !lfnBuffer || bufferSize == 0) {
        return false;
    }
    
    // Find the 8.3 entry (last valid entry)
    int entryIndex = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (entries[i].name[0] != 0x00 && entries[i].name[0] != 0xE5 &&
            entries[i].attributes != FAT32_ATTR_LONG_NAME) {
            entryIndex = i;
            break;
        }
    }
    
    if (entryIndex < 0) {
        return false;
    }
    
    // Check if there are LFN entries before this one
    int lfnCount = 0;
    for (int i = entryIndex - 1; i >= 0; i--) {
        if (entries[i].attributes == FAT32_ATTR_LONG_NAME) {
            lfnCount++;
        } else {
            break;
        }
    }
    
    if (lfnCount == 0) {
        return false;  // No LFN entries
    }
    
    // Verify checksum
    FAT32_LFNEntry* lfnEntries = (FAT32_LFNEntry*)entries;
    uint8_t checksum = calculateLFNChecksum(entries[entryIndex].name);
    
    // LFN entries are stored in reverse order (highest sequence number first)
    // We need to read them from highest to lowest sequence number
    // But they're physically stored from entryIndex-lfnCount to entryIndex-1
    // Sequence numbers go from lfnCount down to 1
    
    // First, collect all LFN entries and sort by sequence number
    // LFN entries are stored physically in reverse order (highest sequence first)
    // We need to sort them so sequence 1 is first
    FAT32_LFNEntry* sortedLFN[20];  // Max 20 LFN entries
    memset(sortedLFN, 0, sizeof(sortedLFN));
    int sortedCount = 0;
    
    for (int i = entryIndex - lfnCount; i < entryIndex; i++) {
        FAT32_LFNEntry* lfn = &lfnEntries[i];
        
        // Verify checksum
        if (lfn->checksum != checksum) {
            return false;  // Checksum mismatch
        }
        
        // Verify it's a LFN entry
        if (lfn->attributes != FAT32_ATTR_LONG_NAME) {
            return false;
        }
        
        // Get sequence number (lower 5 bits, bit 6 indicates last entry)
        uint8_t seq = lfn->sequence & 0x1F;
        if (seq == 0 || seq > lfnCount) {
            return false;  // Invalid sequence
        }
        
        // Store in sorted array (sequence 1 goes to index 0, sequence 2 to index 1, etc.)
        // seq is 1-based, array is 0-based
        sortedLFN[seq - 1] = lfn;
        sortedCount++;
    }
    
    if (sortedCount != lfnCount) {
        return false;  // Missing some entries
    }
    
    // Verify all entries are present
    for (int i = 0; i < lfnCount; i++) {
        if (sortedLFN[i] == nullptr) {
            return false;  // Missing sequence entry
        }
    }
    
    // Now read in correct order (sequence 1 to lfnCount)
    memset(lfnBuffer, 0, bufferSize);
    int bufferPos = 0;
    
    for (int seq = 0; seq < lfnCount; seq++) {
        FAT32_LFNEntry* lfn = sortedLFN[seq];
        if (!lfn) {
            return false;  // Missing entry
        }
        
        // Extract characters from name1 (5 chars)
        for (int j = 0; j < 5 && bufferPos < bufferSize - 1; j++) {
            if (lfn->name1[j] == 0x0000 || lfn->name1[j] == 0xFFFF) {
                break;  // End of name
            }
            // Convert UTF-16 to ASCII (simplified - only handles ASCII range)
            if (lfn->name1[j] < 0x80) {
                lfnBuffer[bufferPos++] = (char)lfn->name1[j];
            } else {
                lfnBuffer[bufferPos++] = '?';  // Non-ASCII character
            }
        }
        
        // Extract characters from name2 (6 chars)
        for (int j = 0; j < 6 && bufferPos < bufferSize - 1; j++) {
            if (lfn->name2[j] == 0x0000 || lfn->name2[j] == 0xFFFF) {
                break;
            }
            if (lfn->name2[j] < 0x80) {
                lfnBuffer[bufferPos++] = (char)lfn->name2[j];
            } else {
                lfnBuffer[bufferPos++] = '?';
            }
        }
        
        // Extract characters from name3 (2 chars)
        for (int j = 0; j < 2 && bufferPos < bufferSize - 1; j++) {
            if (lfn->name3[j] == 0x0000 || lfn->name3[j] == 0xFFFF) {
                break;
            }
            if (lfn->name3[j] < 0x80) {
                lfnBuffer[bufferPos++] = (char)lfn->name3[j];
            } else {
                lfnBuffer[bufferPos++] = '?';
            }
        }
    }
    
    // Trim trailing spaces and nulls
    while (bufferPos > 0 && (lfnBuffer[bufferPos - 1] == ' ' || lfnBuffer[bufferPos - 1] == 0)) {
        bufferPos--;
    }
    lfnBuffer[bufferPos] = 0;  // Null terminate
    
    return true;
}

// Find file in directory (supports both 8.3 and LFN)
bool FAT32::findFileInDirectory(uint32_t dirCluster, const char* filename, FAT32_DirEntry* entry) {
    if (!sdCard || !entry) {
        return false;
    }
    
    // Use static buffer to avoid stack overflow (16KB is too large for stack)
    static uint8_t clusterBuffer[512 * 32];  // Max cluster size (32 sectors = 16KB)
    uint32_t currentCluster = dirCluster;
    
    // Format filename to 8.3 for comparison
    char name83[11];
    format83Name(filename, name83);
    
    // Convert filename to lowercase for case-insensitive comparison
    char filenameLower[256];
    strncpy(filenameLower, filename, sizeof(filenameLower) - 1);
    filenameLower[sizeof(filenameLower) - 1] = 0;
    for (int i = 0; filenameLower[i]; i++) {
        filenameLower[i] = tolower(filenameLower[i]);
    }
    
    // Search through directory clusters
    while (currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX) {
        
        // Read cluster
        if (!readCluster(currentCluster, clusterBuffer)) {
            return false;
        }
        
        // Search entries in this cluster
        int entriesPerCluster = bytesPerCluster / 32;  // 32 bytes per entry
        FAT32_DirEntry* dirEntries = (FAT32_DirEntry*)clusterBuffer;
        
        for (int i = 0; i < entriesPerCluster; i++) {
            // Check if entry is valid (first byte != 0x00 and != 0xE5)
            if (dirEntries[i].name[0] == 0x00) {
                return false;  // End of directory
            }
            
            if (dirEntries[i].name[0] == 0xE5) {
                continue;  // Deleted entry
            }
            
            // Check if it's a long name entry - skip for now, we'll handle it below
            if (dirEntries[i].attributes == FAT32_ATTR_LONG_NAME) {
                continue;
            }
            
            // Try to read LFN if present
            char lfnName[256];
            bool hasLFN = false;
            
            // Check if there are LFN entries before this one
            int lfnCount = 0;
            for (int j = i - 1; j >= 0 && j >= i - 20; j--) {  // Max 20 LFN entries
                if (dirEntries[j].attributes == FAT32_ATTR_LONG_NAME) {
                    lfnCount++;
                } else {
                    break;
                }
            }
            
            if (lfnCount > 0) {
                // Read LFN entries
                hasLFN = readLFNEntries(&dirEntries[i - lfnCount], lfnCount + 1, lfnName, sizeof(lfnName));
                if (hasLFN) {
                    // Convert to lowercase for comparison
                    for (int k = 0; lfnName[k]; k++) {
                        lfnName[k] = tolower(lfnName[k]);
                    }
                }
            }
            
            // Compare with 8.3 name
            bool match83 = (memcmp(dirEntries[i].name, name83, 11) == 0);
            
            // Compare with LFN name
            bool matchLFN = hasLFN && (strcmp(lfnName, filenameLower) == 0);
            
            if (match83 || matchLFN) {
                *entry = dirEntries[i];
                return true;
            }
        }
        
        // Get next cluster
        currentCluster = readFATEntry(currentCluster);
        
        // Check if we've reached end of chain
        if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
            break;
        }
    }
    
    return false;
}

// Find file in current directory
bool FAT32::findFile(const char* filename, FAT32_DirEntry* entry) {
    return findFileInDirectory(currentDirCluster, filename, entry);
}

// Find file with LFN support (returns LFN name if available)
bool FAT32::findFileWithLFN(const char* filename, FAT32_DirEntry* entry, char* lfnName, uint32_t lfnSize) {
    if (!entry || !lfnName || lfnSize == 0) {
        return false;
    }
    
    // First find the file
    if (!findFileInDirectory(currentDirCluster, filename, entry)) {
        return false;
    }
    
    // Try to read LFN (this is a simplified version - in real implementation,
    // we'd need to track the directory entry position)
    lfnName[0] = 0;  // Default to empty (will use 8.3 name)
    
    return true;
}

// Change directory
bool FAT32::changeDirectory(const char* dirname) {
    if (!sdCard) {
        return false;
    }
    
    // Handle special cases
    if (strcmp(dirname, "/") == 0 || strcmp(dirname, "\\") == 0) {
        // Go to root
        currentDirCluster = bootSector.root_cluster;
        strcpy(currentPath, "/");
        return true;
    }
    
    if (strcmp(dirname, "..") == 0) {
        // Go to parent directory
        if (currentDirCluster == bootSector.root_cluster) {
            // Already at root
            return true;
        }
        // For now, just go back to root (parent directory tracking would need more complexity)
        currentDirCluster = bootSector.root_cluster;
        strcpy(currentPath, "/");
        return true;
    }
    
    // Find directory entry
    FAT32_DirEntry entry;
    if (!findFileInDirectory(currentDirCluster, dirname, &entry)) {
        return false;
    }
    
    // Check if it's a directory
    if (!(entry.attributes & FAT32_ATTR_DIRECTORY)) {
        return false;  // Not a directory
    }
    
    // Get directory cluster
    uint32_t dirCluster = entry.cluster_low | (entry.cluster_high << 16);
    if (dirCluster == 0) {
        dirCluster = bootSector.root_cluster;
    }
    
    // Update current directory
    currentDirCluster = dirCluster;
    
    // Update path
    if (strcmp(currentPath, "/") != 0) {
        strcat(currentPath, "/");
    }
    strncat(currentPath, dirname, sizeof(currentPath) - strlen(currentPath) - 1);
    
    return true;
}

// Get current directory path
bool FAT32::getCurrentDirectory(char* path, uint32_t maxSize) {
    if (!path || maxSize == 0) {
        return false;
    }
    
    strncpy(path, currentPath, maxSize - 1);
    path[maxSize - 1] = 0;
    return true;
}

// Set current directory cluster (internal use)
void FAT32::setCurrentDirectory(uint32_t cluster) {
    currentDirCluster = cluster;
}

// Get current directory cluster
uint32_t FAT32::getCurrentDirectoryCluster() const {
    return currentDirCluster;
}

// Check if file exists
bool FAT32::fileExists(const char* filename) {
    FAT32_DirEntry entry;
    return findFile(filename, &entry);
}

// Get file size
uint32_t FAT32::getFileSize(const char* filename) {
    FAT32_DirEntry entry;
    if (findFile(filename, &entry)) {
        return entry.file_size;
    }
    return 0;
}

// Get volume label (returns static string)
const char* FAT32::getVolumeLabel() const {
    static char label[12];
    memcpy(label, bootSector.volume_label, 11);
    label[11] = '\0';
    // Trim trailing spaces
    for (int i = 10; i >= 0 && label[i] == ' '; i--) {
        label[i] = '\0';
    }
    return label;
}

// Get total size in MB
uint32_t FAT32::getTotalSizeMB() const {
    uint32_t totalSectors = bootSector.total_sectors_32;
    if (totalSectors == 0) {
        totalSectors = bootSector.total_sectors_16;
    }
    // Calculate MB: sectors * 512 / (1024 * 1024) = sectors / 2048
    return totalSectors / 2048;
}

// Read file
bool FAT32::readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (!sdCard || !buffer) {
        return false;
    }
    
    FAT32_DirEntry entry;
    if (!findFile(filename, &entry)) {
        return false;
    }
    
    // Get file size
    uint32_t fileSize = entry.file_size;
    if (fileSize > maxSize) {
        fileSize = maxSize;
    }
    
    // Get starting cluster
    uint32_t cluster = entry.cluster_low | (entry.cluster_high << 16);
    
    uint32_t bytesReadSoFar = 0;
    // Use static buffer to avoid stack overflow (16KB is too large for stack)
    static uint8_t clusterBuffer[512 * 32];  // Max cluster size
    
    // Read file cluster by cluster
    while (cluster >= FAT32_CLUSTER_RESERVED_MIN && 
           cluster <= FAT32_CLUSTER_RESERVED_MAX &&
           bytesReadSoFar < fileSize) {
        
        // Read cluster
        if (!readCluster(cluster, clusterBuffer)) {
            break;
        }
        
        // Copy data from cluster to buffer
        uint32_t bytesToCopy = bytesPerCluster;
        if (bytesReadSoFar + bytesToCopy > fileSize) {
            bytesToCopy = fileSize - bytesReadSoFar;
        }
        
        memcpy(buffer + bytesReadSoFar, clusterBuffer, bytesToCopy);
        bytesReadSoFar += bytesToCopy;
        
        // Get next cluster
        cluster = readFATEntry(cluster);
        
        // Check if we've reached end of file
        if (cluster >= FAT32_CLUSTER_EOF_MIN) {
            break;
        }
    }
    
    if (bytesRead) {
        *bytesRead = bytesReadSoFar;
    }
    
    return bytesReadSoFar > 0;
}

// Read file at specific offset
bool FAT32::readFileAtOffset(const char* filename, uint32_t offset, uint8_t* buffer, uint32_t size, uint32_t* bytesRead) {
    //printf("readFileAtOffset: filename='%s', offset=%u, size=%u\r\n",
    //       filename ? filename : "(null)", offset, size);
    
    if (!sdCard || !buffer || size == 0) {
        printf("readFileAtOffset: Invalid parameters - sdCard=%p, buffer=%p, size=%u\r\n",
               sdCard, buffer, size);
        if (bytesRead) *bytesRead = 0;
        return false;
    }
    
    FAT32_DirEntry entry;
    if (!findFile(filename, &entry)) {
        printf("readFileAtOffset: File '%s' not found\r\n", filename);
        if (bytesRead) *bytesRead = 0;
        return false;
    }
    
    //printf("readFileAtOffset: File found - size=%u, start_cluster=%u\r\n", 
    //       entry.file_size, entry.cluster_low | (entry.cluster_high << 16));
    
    // Check if offset is within file bounds
    if (offset >= entry.file_size) {
        //printf("readFileAtOffset: Offset %u >= file size %u\r\n", offset, entry.file_size);
        if (bytesRead) *bytesRead = 0;
        return false;
    }
    
    // Limit size to remaining file data
    uint32_t maxSize = entry.file_size - offset;
    if (size > maxSize) {
        size = maxSize;
    }
    
    // Get starting cluster
    uint32_t cluster = entry.cluster_low | (entry.cluster_high << 16);
    
    // Calculate which cluster contains the offset
    uint32_t clusterOffset = 0;
    uint32_t currentCluster = cluster;
    uint32_t bytesPerCluster = this->bytesPerCluster;
    
    // Find the cluster containing the offset
    int clusterCount = 0;
    while (clusterOffset + bytesPerCluster <= offset && 
           currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX) {
        clusterOffset += bytesPerCluster;
        currentCluster = readFATEntry(currentCluster);
        clusterCount++;
        
        if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
            if (bytesRead) *bytesRead = 0;
            return false;
        }
        
        if (clusterCount > 100) {
            if (bytesRead) *bytesRead = 0;
            return false;
        }
    }
    
    // Verify cluster is valid
    if (currentCluster < FAT32_CLUSTER_RESERVED_MIN || 
        currentCluster > FAT32_CLUSTER_RESERVED_MAX ||
        currentCluster >= FAT32_CLUSTER_EOF_MIN) {
        if (bytesRead) *bytesRead = 0;
        return false;
    }
    
    // Calculate offset within cluster
    uint32_t offsetInCluster = offset - clusterOffset;
    
    // Read data sector by sector
    uint32_t bytesReadSoFar = 0;
    static uint8_t sectorBuffer[512];
    
    while (bytesReadSoFar < size && 
           currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX) {
        
        // Calculate which sector in cluster we're reading from
        uint32_t sectorInCluster = offsetInCluster / 512;
        uint32_t offsetInSector = offsetInCluster % 512;
        
        // Check if sectorInCluster is within cluster bounds
        if (sectorInCluster >= sectorsPerCluster) {
            // Move to next cluster
            uint32_t nextCluster = readFATEntry(currentCluster);
            if (nextCluster >= FAT32_CLUSTER_EOF_MIN || 
                nextCluster < FAT32_CLUSTER_RESERVED_MIN || 
                nextCluster > FAT32_CLUSTER_RESERVED_MAX) {
                break;
            }
            currentCluster = nextCluster;
            offsetInCluster = 0;
            sectorInCluster = 0;
            offsetInSector = 0;
        }
        
        // Verify cluster is still valid
        if (currentCluster < FAT32_CLUSTER_RESERVED_MIN || 
            currentCluster > FAT32_CLUSTER_RESERVED_MAX) {
            break;
        }
        
        // Get sector number
        uint32_t startSector = getClusterSector(currentCluster);
        uint32_t sectorToRead = startSector + sectorInCluster;
        
        // Verify sector is within cluster bounds
        if (sectorInCluster >= sectorsPerCluster) {
            break;
        }
        
        // Read sector
        if (!sdCard->readBlock(sectorToRead, sectorBuffer)) {
            printf("readFileAtOffset: Failed to read sector %u\r\n", sectorToRead);
            break;
        }
        
        // Calculate how many bytes to read from this sector
        uint32_t bytesToRead = 512 - offsetInSector;
        if (bytesToRead > (size - bytesReadSoFar)) {
            bytesToRead = size - bytesReadSoFar;
        }
        
        // Copy data from sector buffer
        memcpy(buffer + bytesReadSoFar, sectorBuffer + offsetInSector, bytesToRead);
        bytesReadSoFar += bytesToRead;
        
        // Move to next sector/cluster if needed
        if (bytesReadSoFar < size) {
            // Move to next sector
            offsetInCluster += bytesToRead;
            
            // Check if we've moved to next cluster
            if (offsetInCluster >= bytesPerCluster) {
                // Move to next cluster
                currentCluster = readFATEntry(currentCluster);
                if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
                    break;
                }
                offsetInCluster = 0;  // Reset offset for new cluster
            }
        }
    }
    
    if (bytesRead) {
        *bytesRead = bytesReadSoFar;
    }
    
    //printf("readFileAtOffset: Completed - bytesReadSoFar=%u, requested=%u, success=%d\r\n",
    //       bytesReadSoFar, size, bytesReadSoFar > 0);
    
    return bytesReadSoFar > 0;
}

// Write file at specific offset
// This function writes data to an existing file at a specific byte offset
bool FAT32::writeFileAtOffset(const char* filename, uint32_t offset, const uint8_t* buffer, uint32_t size) {
    if (!sdCard || !buffer || size == 0) {
        return false;
    }
    
    FAT32_DirEntry entry;
    if (!findFile(filename, &entry)) {
        return false;  // File not found
    }
    
    // Check if file is large enough
    if (entry.file_size < (offset + size)) {
        return false;
    }
    
    // Get starting cluster
    uint32_t cluster = entry.cluster_low | (entry.cluster_high << 16);
    
    // Calculate which cluster contains the offset
    uint32_t clusterOffset = 0;
    uint32_t currentCluster = cluster;
    uint32_t bytesPerCluster = this->bytesPerCluster;
    uint32_t sectorsPerCluster = this->sectorsPerCluster;
    
    // Find the cluster containing the offset
    int clusterCount = 0;
    while (clusterOffset + bytesPerCluster <= offset && 
           currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX) {
        clusterOffset += bytesPerCluster;
        currentCluster = readFATEntry(currentCluster);
        clusterCount++;
        
        if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
            return false;
        }
        
        if (clusterCount > 100) {
            return false;
        }
    }
    
    // Verify cluster is valid
    if (currentCluster < FAT32_CLUSTER_RESERVED_MIN || 
        currentCluster > FAT32_CLUSTER_RESERVED_MAX ||
        currentCluster >= FAT32_CLUSTER_EOF_MIN) {
        return false;
    }
    
    // Now currentCluster contains the cluster we need to write to
    // Calculate offset within cluster
    uint32_t offsetInCluster = offset - clusterOffset;
    
    // Verify offsetInCluster is within cluster bounds
    if (offsetInCluster >= bytesPerCluster) {
        return false;
    }
    
    // Write data sector by sector
    uint32_t bytesWritten = 0;
    static uint8_t sectorBuffer[512];
    
    while (bytesWritten < size && 
           currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX) {
        
        // Calculate which sector in cluster we're writing to
        uint32_t sectorInCluster = offsetInCluster / 512;
        uint32_t offsetInSector = offsetInCluster % 512;
        
        // Check if sectorInCluster is within cluster bounds
        if (sectorInCluster >= sectorsPerCluster) {
            // Move to next cluster
            uint32_t nextCluster = readFATEntry(currentCluster);
            if (nextCluster >= FAT32_CLUSTER_EOF_MIN || 
                nextCluster < FAT32_CLUSTER_RESERVED_MIN || 
                nextCluster > FAT32_CLUSTER_RESERVED_MAX) {
                break;
            }
            currentCluster = nextCluster;
            offsetInCluster = 0;
            sectorInCluster = 0;
            offsetInSector = 0;
        }
        
        // Verify cluster is still valid
        if (currentCluster < FAT32_CLUSTER_RESERVED_MIN || 
            currentCluster > FAT32_CLUSTER_RESERVED_MAX) {
            break;
        }
        
        // Get sector number
        uint32_t startSector = getClusterSector(currentCluster);
        uint32_t sectorToWrite = startSector + sectorInCluster;
        
        // Verify sector is within cluster bounds
        if (sectorInCluster >= sectorsPerCluster) {
            break;
        }
        
        // Always read sector first to preserve existing data
        // Try multiple times in case of transient read errors
        bool readSuccess = false;
        for (int retry = 0; retry < 3; retry++) {
            if (sdCard->readBlock(sectorToWrite, sectorBuffer)) {
                readSuccess = true;
                break;
            }
        }
        
        if (!readSuccess) {
            return false;
        }
        
        // Calculate how many bytes to write in this sector
        uint32_t bytesToWrite = 512 - offsetInSector;
        if (bytesToWrite > (size - bytesWritten)) {
            bytesToWrite = size - bytesWritten;
        }
        
        // Copy data to sector buffer
        memcpy(sectorBuffer + offsetInSector, buffer + bytesWritten, bytesToWrite);
        
        // Write sector back
        if (!sdCard->writeBlock(sectorToWrite, sectorBuffer)) {
            return false;
        }
        
        bytesWritten += bytesToWrite;
        
        // Move to next sector/cluster if needed
        if (bytesWritten < size) {
            // Move to next sector
            offsetInCluster += bytesToWrite;
            
            // Check if we've moved to next cluster
            if (offsetInCluster >= bytesPerCluster) {
                // Move to next cluster
                currentCluster = readFATEntry(currentCluster);
                if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
                    break;
                }
                offsetInCluster = 0;  // Reset offset for new cluster
            }
        }
    }
    
    if (bytesWritten != size) {
        return false;
    }
    
    return true;
}

// List files in current directory
// Structure to hold file entry for sorting
// Using smaller name buffer (64 bytes) to save RAM - sufficient for most filenames
struct FileEntry {
    char name[64];
    bool isDirectory;
};

// Case-insensitive string comparison for sorting
static int strcasecmp_sort(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (*s2 + 32) : *s2;
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Helper function to check if file has allowed extension (case-insensitive)
static bool hasAllowedExtension(const char* filename, const char* ext1, const char* ext2) {
    if (!filename) return false;
    
    // Find the last dot in filename
    const char* lastDot = strrchr(filename, '.');
    if (!lastDot) return false;  // No extension found
    
    // Get extension (skip the dot)
    const char* ext = lastDot + 1;
    
    // Case-insensitive comparison with first extension
    int i = 0;
    while (ext[i] != '\0' && ext1[i] != '\0') {
        char c1 = (ext[i] >= 'A' && ext[i] <= 'Z') ? (ext[i] + 32) : ext[i];
        char c2 = (ext1[i] >= 'A' && ext1[i] <= 'Z') ? (ext1[i] + 32) : ext1[i];
        if (c1 != c2) break;
        i++;
    }
    if (ext[i] == '\0' && ext1[i] == '\0') return true;
    
    // Case-insensitive comparison with second extension
    i = 0;
    while (ext[i] != '\0' && ext2[i] != '\0') {
        char c1 = (ext[i] >= 'A' && ext[i] <= 'Z') ? (ext[i] + 32) : ext[i];
        char c2 = (ext2[i] >= 'A' && ext2[i] <= 'Z') ? (ext2[i] + 32) : ext2[i];
        if (c1 != c2) break;
        i++;
    }
    if (ext[i] == '\0' && ext2[i] == '\0') return true;
    
    return false;
}

bool FAT32::listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount) {
    if (!sdCard || !fileList) {
        return false;
    }
    
    // Use static buffer to avoid stack overflow (16KB is too large for stack)
    static uint8_t clusterBuffer[512 * 32];
    // Static array to hold file entries for sorting (max 64 entries, ~4KB total)
    // Reduced from 256 to save RAM - sufficient for most directories
    static FileEntry entries[64];
    uint32_t entryCount = 0;
    uint32_t currentCluster = currentDirCluster;
    
    // First pass: collect all entries
    while (currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX &&
           entryCount < 64) {  // Max 64 entries
        
        // Read cluster
        if (!readCluster(currentCluster, clusterBuffer)) {
            break;
        }
        
        // Search entries in this cluster
        int entriesPerCluster = bytesPerCluster / 32;
        FAT32_DirEntry* dirEntries = (FAT32_DirEntry*)clusterBuffer;
        
        for (int i = 0; i < entriesPerCluster; i++) {
            // Check if entry is valid
            if (dirEntries[i].name[0] == 0x00) {
                // End of directory
                goto sort_and_format;
            }
            
            if (dirEntries[i].name[0] == 0xE5) {
                continue;  // Deleted entry
            }
            
            // Check if it's a long name entry - skip, we'll handle it with the 8.3 entry
            if (dirEntries[i].attributes == FAT32_ATTR_LONG_NAME) {
                continue;
            }
            
            // Check if it's a volume label
            if (dirEntries[i].attributes & FAT32_ATTR_VOLUME_ID) {
                continue;
            }
            
            // Try to read LFN if present
            char lfnName[256];
            bool hasLFN = false;
            
            // Check if there are LFN entries before this one
            int lfnCount = 0;
            for (int j = i - 1; j >= 0 && j >= i - 20; j--) {  // Max 20 LFN entries
                if (dirEntries[j].attributes == FAT32_ATTR_LONG_NAME) {
                    lfnCount++;
                } else {
                    break;
                }
            }
            
            if (lfnCount > 0) {
                // Read LFN entries
                hasLFN = readLFNEntries(&dirEntries[i - lfnCount], lfnCount + 1, lfnName, sizeof(lfnName));
            }
            
            // Format filename (use LFN if available, otherwise 8.3)
            char filename[256];
            if (hasLFN) {
                strncpy(filename, lfnName, sizeof(filename) - 1);
                filename[sizeof(filename) - 1] = 0;
            } else {
                // Format 8.3 name
                int nameIdx = 0;
                
                // Copy name (8 chars)
                for (int j = 0; j < 8; j++) {
                    if (dirEntries[i].name[j] != ' ') {
                        filename[nameIdx++] = dirEntries[i].name[j];
                    }
                }
                
                // Add extension if present
                if (dirEntries[i].name[8] != ' ') {
                    filename[nameIdx++] = '.';
                    for (int j = 8; j < 11; j++) {
                        if (dirEntries[i].name[j] != ' ') {
                            filename[nameIdx++] = dirEntries[i].name[j];
                        }
                    }
                }
                filename[nameIdx] = 0;
            }
            
            // Check if it's a directory
            bool isDirectory = (dirEntries[i].attributes & FAT32_ATTR_DIRECTORY) != 0;
            
            // Directories are always shown, files are filtered by extension
            if (!isDirectory) {
                // Filter: only show files with .dsk or .nic extension (case-insensitive)
                if (!hasAllowedExtension(filename, "dsk", "nic")) {
                    continue;
                }
            }
            
            // Store entry for sorting
            if (entryCount < 64) {
                strncpy(entries[entryCount].name, filename, sizeof(entries[entryCount].name) - 1);
                entries[entryCount].name[sizeof(entries[entryCount].name) - 1] = 0;
                entries[entryCount].isDirectory = isDirectory;
                entryCount++;
            }
        }
        
        // Get next cluster
        currentCluster = readFATEntry(currentCluster);
        
        if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
            break;
        }
    }
    
sort_and_format:
    // Sort entries: directories first, then files, both alphabetically
    // Simple bubble sort (efficient enough for small lists)
    for (uint32_t i = 0; i < entryCount - 1; i++) {
        for (uint32_t j = 0; j < entryCount - i - 1; j++) {
            bool swap = false;
            
            // Directories come before files
            if (entries[j].isDirectory && !entries[j + 1].isDirectory) {
                swap = false;  // Already in correct order (dir before file)
            } else if (!entries[j].isDirectory && entries[j + 1].isDirectory) {
                swap = true;  // Need to swap (file before dir)
            } else {
                // Both are same type, sort alphabetically
                if (strcasecmp_sort(entries[j].name, entries[j + 1].name) > 0) {
                    swap = true;
                }
            }
            
            if (swap) {
                // Swap entries
                FileEntry temp = entries[j];
                entries[j] = entries[j + 1];
                entries[j + 1] = temp;
            }
        }
    }
    
    // Format sorted entries into output string
    uint32_t listPos = 0;
    for (uint32_t i = 0; i < entryCount && listPos < maxSize - 64; i++) {
        const char* typeStr = entries[i].isDirectory ? " <DIR>" : "";
        int len = snprintf(fileList + listPos, maxSize - listPos, 
                         "%s%s\r\n", entries[i].name, typeStr);
        if (len > 0 && listPos + len < maxSize) {
            listPos += len;
        }
    }
    
    if (fileCount) {
        *fileCount = entryCount;
    }
    fileList[listPos] = 0;
    return true;
}

