#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <time.h>
#include "cm256.h"

#define BLOCK_BYTES 4096
#define ORIGINAL_COUNT 4
#define RECOVERY_COUNT 4

void hexDump (char *desc, void *addr, uint32_t len) {
    int i;
    uint8_t buff[17];
    uint8_t *pc = (uint8_t*)addr;

    // Output description if given.
    if (desc != NULL)
        printf ("%s:\n", desc);

    if (len == 0) {
        printf("  ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printf("  NEGATIVE LENGTH: %i\n",len);
        return;
    }

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0)
                printf ("  %s\n", buff);

            // Output the offset.
            printf ("  %04x ", i);
        }

        // Now the hex code for the specific character.
        printf (" %02x", pc[i]);

        // And store a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        printf ("   ");
        i++;
    }

    // And print the final ASCII bit.
    printf ("  %s\n", buff);
}

int ExampleFileUsage()
{   
    clock_t start, end;
    if (cm256_init())
    {
        printf("Initialization messed up\n");
        return 1;
    }
    printf("Initialized\n");

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
    uint8_t* filedatacopy = malloc(OriginalFileBytes);

    //Doing it this way as some Linux versions don't support a normal get_random
    syscall(SYS_getrandom, originalFileData, OriginalFileBytes, 0);
    //hexDump("data", originalFileData, 4096);
    memcpy(filedatacopy, originalFileData, OriginalFileBytes);

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
    start = clock();
    if (cm256_encode(params, blocks, recoveryBlocks))
    {
        return 1;
    }
    end = clock();
    double total_time = ((double) (end - start))/ CLOCKS_PER_SEC;
    printf("Time to run encode %f\n", total_time);

    //hexDump("parity", recoveryBlocks, 4096);

    // Initialize the indices
    for (int i = 0; i < params.OriginalCount; ++i)
    {
        blocks[i].Index = cm256_get_original_block_index(params, i);
    }

    //// Simulate loss of data, subsituting a recovery block in its place ////
    blocks[0].Block = &recoveryBlocks[0]; // First recovery block
    blocks[0].Index = cm256_get_recovery_block_index(params, 0); // First recovery block index
    //// Simulate loss of data, subsituting a recovery block in its place ////
    
    blocks[1].Block = &recoveryBlocks[4096];
    blocks[1].Index = cm256_get_recovery_block_index(params, 1);
    

    start = clock();
    int ret;
    if (ret = cm256_decode(params, blocks))
    {
	printf("decode failed %d \n", ret);
        return 1;
    }
    end = clock();
    total_time = ((double) (end - start))/ CLOCKS_PER_SEC;
    printf("Time to run decode %f\n", total_time);

    // blocks[0].Index will now be 0.
    //hexDump("regenerated", blocks[0].Block, 4096);
    
    for(int i = 0; i < 2 * 4096; i++){
        if(blocks[i/4096].Block[i%4096] != filedatacopy[i]){
            printf("Decode errors on byte %d\n", i);
	    return -1;
	}
    }


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
