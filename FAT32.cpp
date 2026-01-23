#include "FAT32.h"
#include "SDCardManager.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Constructor
FAT32::FAT32(SDCardManager* sdCardManager) {
    sdCard = sdCardManager;
    memset(&bootSector, 0, sizeof(bootSector));
    fatStartSector = 0;
    dataStartSector = 0;
    rootDirStartSector = 0;
    bytesPerCluster = 0;
    sectorsPerCluster = 0;
    currentDirCluster = 0;
    memset(currentPath, 0, sizeof(currentPath));
    strcpy(currentPath, "/");
}

// Initialize FAT32 filesystem
bool FAT32::init() {
    if (!sdCard) {
        return false;
    }
    
    // Read boot sector (sector 0)
    uint8_t bootSectorBuffer[512];
    if (!sdCard->readBlock(0, bootSectorBuffer)) {
        return false;
    }
    
    memcpy(&bootSector, bootSectorBuffer, sizeof(FAT32_BootSector));
    
    // Verify FAT32 signature
    if (bootSector.bytes_per_sector != 512) {
        return false;  // Only support 512 byte sectors
    }
    
    // Check if it's FAT32 (fs_type should contain "FAT32")
    if (strncmp((char*)bootSector.fs_type, "FAT32", 5) != 0 && 
        strncmp((char*)bootSector.fs_type, "FAT", 3) != 0) {
        // Try to detect by sectors_per_fat_32
        if (bootSector.sectors_per_fat_32 == 0) {
            return false;  // Not FAT32
        }
    }
    
    // Calculate FAT32 structure
    sectorsPerCluster = bootSector.sectors_per_cluster;
    bytesPerCluster = bootSector.bytes_per_sector * sectorsPerCluster;
    
    // FAT start sector
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

// Get sector number for a cluster
uint32_t FAT32::getClusterSector(uint32_t cluster) {
    // First data cluster is 2, so subtract 2
    return dataStartSector + ((cluster - 2) * sectorsPerCluster);
}

// Read FAT entry for a cluster
uint32_t FAT32::readFATEntry(uint32_t cluster) {
    if (!sdCard) {
        return FAT32_CLUSTER_EOF_MIN;
    }
    
    // FAT32 uses 4 bytes per entry
    uint32_t fatOffset = cluster * 4;
    uint32_t fatSector = fatStartSector + (fatOffset / 512);
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
    printf("readFileAtOffset: filename='%s', offset=%u, size=%u\r\n",
           filename ? filename : "(null)", offset, size);
    
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
    
    printf("readFileAtOffset: File found - size=%u, start_cluster=%u\r\n", 
           entry.file_size, entry.cluster_low | (entry.cluster_high << 16));
    
    // Check if offset is within file bounds
    if (offset >= entry.file_size) {
        printf("readFileAtOffset: Offset %u >= file size %u\r\n", offset, entry.file_size);
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
    
    printf("readFileAtOffset: Completed - bytesReadSoFar=%u, requested=%u, success=%d\r\n",
           bytesReadSoFar, size, bytesReadSoFar > 0);
    
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
bool FAT32::listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount) {
    if (!sdCard || !fileList) {
        return false;
    }
    
    // Use static buffer to avoid stack overflow (16KB is too large for stack)
    static uint8_t clusterBuffer[512 * 32];
    uint32_t currentCluster = currentDirCluster;
    uint32_t count = 0;
    uint32_t listPos = 0;
    
    // Search through root directory clusters
    while (currentCluster >= FAT32_CLUSTER_RESERVED_MIN && 
           currentCluster <= FAT32_CLUSTER_RESERVED_MAX &&
           listPos < maxSize - 64) {  // Leave room for entry
        
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
                if (fileCount) {
                    *fileCount = count;
                }
                fileList[listPos] = 0;
                return true;
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
            
            // Add directory indicator
            const char* typeStr = (dirEntries[i].attributes & FAT32_ATTR_DIRECTORY) ? " <DIR>" : "";
            
            // Add to list (filename only, no size)
            int len = snprintf(fileList + listPos, maxSize - listPos, 
                             "%s%s\r\n", filename, typeStr);
            if (len > 0 && listPos + len < maxSize) {
                listPos += len;
                count++;
            }
        }
        
        // Get next cluster
        currentCluster = readFATEntry(currentCluster);
        
        if (currentCluster >= FAT32_CLUSTER_EOF_MIN) {
            break;
        }
    }
    
    if (fileCount) {
        *fileCount = count;
    }
    fileList[listPos] = 0;
    return true;
}

