#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <linux/types.h>
#include "cm256.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AUSTEN BARKER");

#define BLOCK_BYTES 4096
#define ORIGINAL_COUNT 4
#define RECOVERY_COUNT 4

int ExampleFileUsage(void)
{   
    cm256_encoder_params params;
    static const int OriginalFileBytes = ORIGINAL_COUNT * BLOCK_BYTES;
    uint8_t* originalFileData; 
    uint8_t* filedatacopy;
    uint8_t* recoveryBlocks;
    cm256_block *blocks = kmalloc(sizeof(cm256_block) * 256, GFP_KERNEL);
    int i, ret;
    struct timespec timespec1, timespec2;

    if (cm256_init())
    {
        printk(KERN_INFO "Initialization messed up\n");
        return 1;
    }
    printk(KERN_INFO "Initialized\n");

    // Number of bytes per file block
    params.BlockBytes = BLOCK_BYTES;

    // Number of blocks
    params.OriginalCount = ORIGINAL_COUNT;

    // Number of additional recovery blocks generated by encoder
    params.RecoveryCount = RECOVERY_COUNT;

    // Allocate and fill the original file data
    originalFileData = kmalloc(OriginalFileBytes, GFP_KERNEL);
    filedatacopy = kmalloc(OriginalFileBytes, GFP_KERNEL);

    //Doing it this way as some Linux versions don't support a normal get_random
    get_random_bytes(originalFileData, OriginalFileBytes);
    memcpy(filedatacopy, originalFileData, OriginalFileBytes);

    // Pointers to data
    for (i = 0; i < params.OriginalCount; ++i)
    {
        blocks[i].Block = originalFileData + i * params.BlockBytes;
    }

    printk(KERN_INFO "data generated\n");

    // Recovery data
    recoveryBlocks = kmalloc(params.RecoveryCount * params.BlockBytes, GFP_KERNEL);

    // Generate recovery data
    getnstimeofday(&timespec1);
    if (cm256_encode(params, blocks, recoveryBlocks))
    {
        return 1;
    }
    getnstimeofday(&timespec2);
    printk(KERN_INFO "Encode took: %ld nanoseconds",
(timespec2.tv_sec - timespec1.tv_sec) * 1000000000 + (timespec2.tv_nsec - timespec1.tv_nsec));

    // Initialize the indices
    for (i = 0; i < params.OriginalCount; ++i)
    {
        blocks[i].Index = cm256_get_original_block_index(params, i);
    }

    //// Simulate loss of data, subsituting a recovery block in its place ////
    blocks[0].Block = &recoveryBlocks[0]; // First recovery block
    blocks[0].Index = cm256_get_recovery_block_index(params, 0); // First recovery block index
    //// Simulate loss of data, subsituting a recovery block in its place ////
    
    blocks[1].Block = &recoveryBlocks[4096];
    blocks[1].Index = cm256_get_recovery_block_index(params, 1);

    getnstimeofday(&timespec1);    
    ret = cm256_decode(params, blocks);
    getnstimeofday(&timespec2);
    printk(KERN_INFO "Decode took: %ld nanoseconds",
(timespec2.tv_sec - timespec1.tv_sec) * 1000000000 + (timespec2.tv_nsec - timespec1.tv_nsec));\

    if (ret)
    {
	printk(KERN_INFO "decode failed %d \n", ret);
        return 1;
    }

    
    for(i = 0; i < 2 * 4096; i++){
        if(blocks[i/4096].Block[i%4096] != filedatacopy[i]){
            printk(KERN_INFO "Decode errors on byte %d\n", i);
	    return -1;
	}
    }

    printk(KERN_INFO "decode worked\n");


    kfree(originalFileData);
    kfree(filedatacopy);
    kfree(recoveryBlocks);
    kfree(blocks);
    return 0;
}

static int __init km_template_init(void){
    ExampleFileUsage();
    printk(KERN_INFO "Kernel Module inserted");
    return 0;
}

static void __exit km_template_exit(void){
    printk(KERN_INFO "Removing kernel module\n");
}

module_init(km_template_init);
module_exit(km_template_exit);
