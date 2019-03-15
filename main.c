#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "cm256.h"

#define BLOCK_BYTES 4096
#define ORIGINAL_COUNT 2
#define RECOVERY_COUNT 2

int ExampleFileUsage()
{
    printf("Initialize\n");
    if (cm256_init())
    {
        printf("Initialization messed up\n");
        return 1;
    }
    printf("initialized\n");

    cm256_encoder_params params;

    // Number of bytes per file block
    params.BlockBytes = BLOCK_BYTES;

    // Number of blocks
    params.OriginalCount = ORIGINAL_COUNT;

    // Number of additional recovery blocks generated by encoder
    params.RecoveryCount = RECOVERY_COUNT;

    // Size of the original file
    static const int OriginalFileBytes = ORIGINAL_COUNT * BLOCK_BYTES;

    // Allocate and fill the original file data
    uint8_t* originalFileData = malloc(OriginalFileBytes);
    memset(originalFileData, 1, OriginalFileBytes);

    // Pointers to data
    cm256_block blocks[256];
    for (int i = 0; i < params.OriginalCount; ++i)
    {
        blocks[i].Block = originalFileData + i * params.BlockBytes;
    }

    fprintf(stdout, "data generated\n");

    // Recovery data
    uint8_t* recoveryBlocks = malloc(params.RecoveryCount * params.BlockBytes);

    // Generate recovery data
    if (cm256_encode(params, blocks, recoveryBlocks))
    {
        return 1;
    }
    printf("encoded\n");

    // Initialize the indices
    for (int i = 0; i < params.OriginalCount; ++i)
    {
        blocks[i].Index = cm256_get_original_block_index(params, i);
    }

    //// Simulate loss of data, subsituting a recovery block in its place ////
    blocks[0].Block = recoveryBlocks; // First recovery block
    blocks[0].Index = cm256_get_recovery_block_index(params, 0); // First recovery block index
    //// Simulate loss of data, subsituting a recovery block in its place ////

    int ret;
    if (ret = cm256_decode(params, blocks))
    {
	printf("decode failed %d \n", ret);
        return 1;
    }

    printf("seems to have run\nmm");

    // blocks[0].Index will now be 0.

    free(originalFileData);
    free(recoveryBlocks);

    return 0;
}

int main(){
    if(ExampleFileUsage()){
        printf("what the hell?\n");
    }    
    return 0;
}
