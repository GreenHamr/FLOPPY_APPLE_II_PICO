#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

// FAT32 Boot Sector structure
typedef struct {
    uint8_t jump[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;        // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t media_type;
    uint16_t sectors_per_fat_16;  // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed)) FAT32_BootSector;

// FAT32 Directory Entry structure
typedef struct {
    uint8_t name[11];             // 8.3 format
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) FAT32_DirEntry;

// FAT32 Long File Name Entry structure
typedef struct {
    uint8_t sequence;            // Sequence number (bit 6 = last entry)
    uint16_t name1[5];           // First 5 characters (UTF-16)
    uint8_t attributes;          // Always 0x0F for LFN
    uint8_t type;                // Always 0x00
    uint8_t checksum;            // Checksum of short name
    uint16_t name2[6];           // Next 6 characters (UTF-16)
    uint16_t first_cluster;      // Always 0x0000
    uint16_t name3[2];           // Last 2 characters (UTF-16)
} __attribute__((packed)) FAT32_LFNEntry;

// FAT32 File attributes
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  0x0F

// FAT32 special cluster values
#define FAT32_CLUSTER_FREE    0x0000000
#define FAT32_CLUSTER_RESERVED_MIN 0x0000002
#define FAT32_CLUSTER_RESERVED_MAX 0x0FFFFFEF
#define FAT32_CLUSTER_BAD     0x0FFFFFF7
#define FAT32_CLUSTER_EOF_MIN 0x0FFFFFF8
#define FAT32_CLUSTER_EOF_MAX 0x0FFFFFFF

// Forward declaration
class SDCardManager;

class FAT32 {
private:
    // Pointer to SDCardManager for reading blocks
    SDCardManager* sdCard;
    
    // FAT32 structure
    FAT32_BootSector bootSector;
    uint32_t fatStartSector;
    uint32_t dataStartSector;
    uint32_t rootDirStartSector;
    uint32_t bytesPerCluster;
    uint32_t sectorsPerCluster;
    
    // Current directory tracking
    uint32_t currentDirCluster;
    char currentPath[256];
    
    // Internal methods
    bool readBootSector();
    uint32_t getClusterSector(uint32_t cluster);
    uint32_t readFATEntry(uint32_t cluster);
    bool readCluster(uint32_t cluster, uint8_t* buffer);
    bool findFileInDirectory(uint32_t dirCluster, const char* filename, FAT32_DirEntry* entry);
    void format83Name(const char* filename, char* name83);
    bool compare83Name(const char* name83, const char* filename);
    
public:
    // Constructor
    FAT32(SDCardManager* sdCardManager);
    
    // Initialization
    bool init();
    
    // File operations
    bool fileExists(const char* filename);
    bool readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    bool listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount);
    
    // Directory operations
    bool findFile(const char* filename, FAT32_DirEntry* entry);
    bool findFileWithLFN(const char* filename, FAT32_DirEntry* entry, char* lfnName, uint32_t lfnSize);
    bool changeDirectory(const char* dirname);
    bool getCurrentDirectory(char* path, uint32_t maxSize);
    void setCurrentDirectory(uint32_t cluster);
    uint32_t getCurrentDirectoryCluster() const;
    
    // LFN support
    bool readLFNEntries(FAT32_DirEntry* entries, int count, char* lfnBuffer, uint32_t bufferSize);
    uint8_t calculateLFNChecksum(const uint8_t* shortName);
    
    // Utility
    uint32_t getFileSize(const char* filename);
};

#endif // FAT32_H

