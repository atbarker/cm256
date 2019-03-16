/*
	Copyright (c) 2015 Christopher A. Taylor.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of CM256 nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#include "cm256.h"
#include <linux/types.h>
#include <linux/slab.h>

/*
    GF(256) Cauchy Matrix Overview

    As described on Wikipedia, each element of a normal Cauchy matrix is defined as:

        a_ij = 1 / (x_i - y_j)
        The arrays x_i and y_j are vector parameters of the matrix.
        The values in x_i cannot be reused in y_j.

    Moving beyond the Wikipedia...

    (1) Number of rows (R) is the range of i, and number of columns (C) is the range of j.

    (2) Being able to select x_i and y_j makes Cauchy matrices more flexible in practice
        than Vandermonde matrices, which only have one parameter per row.

    (3) Cauchy matrices are always invertible, AKA always full rank, AKA when treated as
        as linear system y = M*x, the linear system has a single solution.

    (4) A Cauchy matrix concatenated below a square CxC identity matrix always has rank C,
        Meaning that any R rows can be eliminated from the concatenated matrix and the
        matrix will still be invertible.  This is how Reed-Solomon erasure codes work.

    (5) Any row or column can be multiplied by non-zero values, and the resulting matrix
        is still full rank.  This is true for any matrix, since it is effectively the same
        as pre and post multiplying by diagonal matrices, which are always invertible.

    (6) Matrix elements with a value of 1 are much faster to operate on than other values.
        For instance a matrix of [1, 1, 1, 1, 1] is invertible and much faster for various
        purposes than [2, 2, 2, 2, 2].

    (7) For GF(256) matrices, the symbols in x_i and y_j are selected from the numbers
        0...255, and so the number of rows + number of columns may not exceed 256.
        Note that values in x_i and y_j may not be reused as stated above.

    In summary, Cauchy matrices
        are preferred over Vandermonde matrices.  (2)
        are great for MDS erasure codes.  (3) and (4)
        should be optimized to include more 1 elements.  (5) and (6)
        have a limited size in GF(256), rows+cols <= 256.  (7)
*/


//-----------------------------------------------------------------------------
// Initialization

extern int cm256_init_(int version){
    if (version != CM256_VERSION){
        // User's header does not match library version
        return -10;
    }

    // Return error code from GF(256) init if required
    return gf256_init();
}


/*
    Selected Cauchy Matrix Form

    The matrix consists of elements a_ij, where i = row, j = column.
    a_ij = 1 / (x_i - y_j), where x_i and y_j are sets of GF(256) values
    that do not intersect.

    We select x_i and y_j to just be incrementing numbers for the
    purposes of this library.  Further optimizations may yield matrices
    with more 1 elements, but the benefit seems relatively small.

    The x_i values range from 0...(originalCount - 1).
    The y_j values range from originalCount...(originalCount + recoveryCount - 1).

    We then improve the Cauchy matrix by dividing each column by the
    first row element of that column.  The result is an invertible
    matrix that has all 1 elements in the first row.  This is equivalent
    to a rotated Vandermonde matrix, so we could have used one of those.

    The advantage of doing this is that operations involving the first
    row will be extremely fast (just memory XOR), so the decoder can
    be optimized to take advantage of the shortcut when the first
    recovery row can be used.

    First row element of Cauchy matrix for each column:
    a_0j = 1 / (x_0 - y_j) = 1 / (x_0 - y_j)

    Our Cauchy matrix sets first row to ones, so:
    a_ij = (1 / (x_i - y_j)) / a_0j
    a_ij = (y_j - x_0) / (x_i - y_j)
    a_ij = (y_j + x_0) div (x_i + y_j) in GF(256)
*/

// This function generates each matrix element based on x_i, x_0, y_j
// Note that for x_i == x_0, this will return 1, so it is better to unroll out the first row.
static GF256_FORCE_INLINE unsigned char GetMatrixElement(unsigned char x_i, unsigned char x_0, unsigned char y_j){
    return gf256_div(gf256_add(y_j, x_0), gf256_add(x_i, y_j));
}


//-----------------------------------------------------------------------------
// Encoding

extern void cm256_encode_block(
    cm256_encoder_params params, // Encoder parameters
    cm256_block* originals,      // Array of pointers to original blocks
    int recoveryBlockIndex,      // Return value from cm256_get_recovery_block_index()
    void* recoveryBlock)         // Output recovery block
{   
    uint8_t x_0, x_i, y_0, y_j, matrixElement;
    int j;
    // If only one block of input data,
    if (params.OriginalCount == 1){
        // No meaningful operation here, degenerate to outputting the same data each time.

        memcpy(recoveryBlock, originals[0].Block, params.BlockBytes);
        return;
    }
    // else OriginalCount >= 2:

    // Unroll first row of recovery matrix:
    // The matrix we generate for the first row is all ones,
    // so it is merely a parity of the original data.
    if (recoveryBlockIndex == params.OriginalCount){
        gf256_addset_mem(recoveryBlock, originals[0].Block, originals[1].Block, params.BlockBytes);
        for (j = 2; j < params.OriginalCount; ++j){
            gf256_add_mem(recoveryBlock, originals[j].Block, params.BlockBytes);
        }
        return;
    }

    // TBD: Faster algorithms seem to exist for computing this matrix-vector product.

    // Start the x_0 values arbitrarily from the original count.
    x_0 = (uint8_t)(params.OriginalCount);

    // For other rows:
    {
        x_i = (uint8_t)(recoveryBlockIndex);

        // Unroll first operation for speed
        {
            y_0 = 0;
            matrixElement = GetMatrixElement(x_i, x_0, y_0);

            gf256_mul_mem(recoveryBlock, originals[0].Block, matrixElement, params.BlockBytes);
        }

        // For each original data column,
        for (j = 1; j < params.OriginalCount; ++j){
            y_j = (uint8_t)(j);
            matrixElement = GetMatrixElement(x_i, x_0, y_j);

            gf256_muladd_mem(recoveryBlock, matrixElement, originals[j].Block, params.BlockBytes);
        }
    }
}

extern int cm256_encode(
    cm256_encoder_params params, // Encoder params
    cm256_block* originals,      // Array of pointers to original blocks
    void* recoveryBlocks)        // Output recovery blocks end-to-end
{
    uint8_t* recoveryBlock;
    int block;

    // Validate input:
    if (params.OriginalCount <= 0 || params.RecoveryCount <= 0 || params.BlockBytes <= 0){
        return -1;
    }
    if (params.OriginalCount + params.RecoveryCount > 256){
        return -2;
    }
    if (!originals || !recoveryBlocks){
        return -3;
    }

    recoveryBlock = (uint8_t*)(recoveryBlocks);

    for (block = 0; block < params.RecoveryCount; ++block, recoveryBlock += params.BlockBytes){
        cm256_encode_block(params, originals, (params.OriginalCount + block), recoveryBlock);
    }

    return 0;
}


//-----------------------------------------------------------------------------
// Decoding

typedef struct {
    // Encode parameters
    cm256_encoder_params Params;

    // Recovery blocks
    cm256_block* Recovery[256];
    int RecoveryCount;

    // Original blocks
    cm256_block* Original[256];
    int OriginalCount;

    // Row indices that were erased
    uint8_t ErasuresIndices[256];

    // Initialize the decoder

    // Decode m=1 case

    // Decode for m>1 case

    // Generate the LU decomposition of the matrix
}CM256Decoder;

int Initialize(CM256Decoder *decoder, cm256_encoder_params params, cm256_block* blocks) {
    int ii, row, indexCount;
    cm256_block* block = blocks;
    decoder->Params = params;

    decoder->OriginalCount = 0;
    decoder->RecoveryCount = 0;

    // Initialize erasures to zeros
    for (ii = 0; ii < params.OriginalCount; ++ii){
        decoder->ErasuresIndices[ii] = 0;
    }

    // For each input block,
    for (ii = 0; ii < params.OriginalCount; ++ii, ++block){
        row = block->Index;

        // If it is an original block,
        if (row < params.OriginalCount){
            decoder->Original[decoder->OriginalCount++] = block;
            if (decoder->ErasuresIndices[row] != 0){
                // Error out if two row indices repeat
                printk(KERN_INFO "Indices incorrect\n");
                return -1;
            }
            decoder->ErasuresIndices[row] = 1;
        }
        else{
            decoder->Recovery[decoder->RecoveryCount++] = block;
        }
    }

    // Identify erasures
    for (ii = 0, indexCount = 0; ii < 256; ++ii) {
        if (!decoder->ErasuresIndices[ii]) {
            decoder->ErasuresIndices[indexCount] = (uint8_t)( ii );

            if (++indexCount >= decoder->RecoveryCount) {
                break;
            }
        }
    }
    return 0;
}

void DecodeM1(CM256Decoder *decoder){
    // XOR all other blocks into the recovery block
    uint8_t* outBlock = (uint8_t*)(decoder->Recovery[0]->Block);
    uint8_t* inBlock = NULL;
    int ii;
    uint8_t* inBlock2;

    // For each block,
    for (ii = 0; ii < decoder->OriginalCount; ++ii) {
        inBlock2 = (uint8_t*)(decoder->Original[ii]->Block);

        if (!inBlock) {
            inBlock = inBlock2;
        }else {
            // outBlock ^= inBlock ^ inBlock2
            gf256_add2_mem(outBlock, inBlock, inBlock2, decoder->Params.BlockBytes);
            inBlock = NULL;
        }
    }

    // Complete XORs
    if (inBlock) {
        gf256_add_mem(outBlock, inBlock, decoder->Params.BlockBytes);
    }

    // Recover the index it corresponds to
    decoder->Recovery[0]->Index = decoder->ErasuresIndices[0];
}

// Generate the LU decomposition of the matrix
void GenerateLDUDecomposition(CM256Decoder *decoder, uint8_t* matrix_L, uint8_t* diag_D, uint8_t* matrix_U) {
    // Schur-type-direct-Cauchy algorithm 2.5 from
    // "Pivoting and Backward Stability of Fast Algorithms for Solving Cauchy Linear Equations"
    // T. Boros, T. Kailath, V. Olshevsky
    // Modified for practical use.  I folded the diagonal parts of U/L matrices into the
    // diagonal one to reduce the number of multiplications to perform against the input data,
    // and organized the triangle matrices in memory to allow for faster SSE3 GF multiplications.

    // Matrix size NxN
    int N = decoder->RecoveryCount;
    int count;
    int i, firstOffset_U, j, k;
    uint8_t rotated_row_U[256];
    uint8_t *last_U, *row_L, *row_U, *output_U;
    uint8_t x_0, x_k, y_k, D_kk, L_kk, U_kk, x_j, y_j, L_jk, U_kj, x_n, y_n, L_nn, U_nn;

    // Generators
    uint8_t g[256], b[256];
    for (i = 0; i < N; ++i) {
        g[i] = 1;
        b[i] = 1;
    }

    // Temporary buffer for rotated row of U matrix
    // This allows for faster GF bulk multiplication
    last_U = matrix_U + ((N - 1) * N) / 2 - 1;
    firstOffset_U = 0;

    // Start the x_0 values arbitrarily from the original count.
    x_0 = (uint8_t)(decoder->Params.OriginalCount);

    // Unrolling k = 0 just makes it slower for some reason.
    for (k = 0; k < N - 1; ++k) {
        x_k = decoder->Recovery[k]->Index;
        y_k = decoder->ErasuresIndices[k];

        // D_kk = (x_k + y_k)
        // L_kk = g[k] / (x_k + y_k)
        // U_kk = b[k] * (x_0 + y_k) / (x_k + y_k)
        D_kk = gf256_add(x_k, y_k);
        L_kk = gf256_div(g[k], D_kk);
        U_kk = gf256_mul(gf256_div(b[k], D_kk), gf256_add(x_0, y_k));

        // diag_D[k] = D_kk * L_kk * U_kk
        diag_D[k] = gf256_mul(D_kk, gf256_mul(L_kk, U_kk));

        // Computing the k-th row of L and U
        row_L = matrix_L;
        row_U = rotated_row_U;
        for (j = k + 1; j < N; ++j) {
            x_j = decoder->Recovery[j]->Index;
            y_j = decoder->ErasuresIndices[j];

            // L_jk = g[j] / (x_j + y_k)
            // U_kj = b[j] / (x_k + y_j)
            L_jk = gf256_div(g[j], gf256_add(x_j, y_k));
            U_kj = gf256_div(b[j], gf256_add(x_k, y_j));

            *matrix_L++ = L_jk;
            *row_U++ = U_kj;

            // g[j] = g[j] * (x_j + x_k) / (x_j + y_k)
            // b[j] = b[j] * (y_j + y_k) / (y_j + x_k)
            g[j] = gf256_mul(g[j], gf256_div(gf256_add(x_j, x_k), gf256_add(x_j, y_k)));
            b[j] = gf256_mul(b[j], gf256_div(gf256_add(y_j, y_k), gf256_add(y_j, x_k)));
        }

        // Do these row/column divisions in bulk for speed.
        // L_jk /= L_kk
        // U_kj /= U_kk
        count = N - (k + 1);
        gf256_div_mem(row_L, row_L, L_kk, count);
        gf256_div_mem(rotated_row_U, rotated_row_U, U_kk, count);

        // Copy U matrix row into place in memory.
        output_U = last_U + firstOffset_U;
        row_U = rotated_row_U;
        for (j = k + 1; j < N; ++j) {
            *output_U = *row_U++;
            output_U -= j;
        }
        firstOffset_U -= k + 2;
    }

    // Multiply diagonal matrix into U
    row_U = matrix_U;
    for (j = N - 1; j > 0; --j) {
        y_j = decoder->ErasuresIndices[j];
        count = j;

        gf256_mul_mem(row_U, row_U, gf256_add(x_0, y_j), count);
        row_U += count;
    }

    x_n = decoder->Recovery[N - 1]->Index;
    y_n = decoder->ErasuresIndices[N - 1];

    // D_nn = 1 / (x_n + y_n)
    // L_nn = g[N-1]
    // U_nn = b[N-1] * (x_0 + y_n)
    L_nn = g[N - 1];
    U_nn = gf256_mul(b[N - 1], gf256_add(x_0, y_n));

    // diag_D[N-1] = L_nn * D_nn * U_nn
    diag_D[N - 1] = gf256_div(gf256_mul(L_nn, U_nn), gf256_add(x_n, y_n));
}

void Decode(CM256Decoder *decoder) {
    // Matrix size is NxN, where N is the number of recovery blocks used.
    const int N = decoder->RecoveryCount;

    // Start the x_0 values arbitrarily from the original count.
    const uint8_t x_0 = (uint8_t)(decoder->Params.OriginalCount);

    int originalIndex, recoveryIndex, j, i;
    int requiredSpace;
    uint8_t *inBlock, *outBlock, *dynamicMatrix, *matrix, *matrix_U, *diag_D, *matrix_L;
    uint8_t inRow, x_i, y_j, matrixElement, c_ij;
    static const int StackAllocSize = 2048;
    uint8_t stackMatrix[StackAllocSize];
    void *block_j;
    void *block_i, *block;
    // Eliminate original data from the the recovery rows
    for (originalIndex = 0; originalIndex < decoder->OriginalCount; ++originalIndex) {
        inBlock = (uint8_t*)(decoder->Original[originalIndex]->Block);
        inRow = decoder->Original[originalIndex]->Index;

        for (recoveryIndex = 0; recoveryIndex < N; ++recoveryIndex) {
            outBlock = (uint8_t*)(decoder->Recovery[recoveryIndex]->Block);
            x_i = decoder->Recovery[recoveryIndex]->Index;
            y_j = inRow;
            matrixElement = GetMatrixElement(x_i, x_0, y_j);

            gf256_muladd_mem(outBlock, matrixElement, inBlock, decoder->Params.BlockBytes);
        }
    }

    // Allocate matrix
    dynamicMatrix = NULL;
    matrix = stackMatrix;
    requiredSpace = N * N;
    if (requiredSpace > StackAllocSize) {
        dynamicMatrix = kmalloc(requiredSpace, GFP_KERNEL);
        matrix = dynamicMatrix;
    }

    /*
        Compute matrix decomposition:

            G = L * D * U

        L is lower-triangular, diagonal is all ones.
        D is a diagonal matrix.
        U is upper-triangular, diagonal is all ones.
    */
    matrix_U = matrix;
    diag_D = matrix_U + (N - 1) * N / 2;
    matrix_L = diag_D + N;
    GenerateLDUDecomposition(decoder, matrix_L, diag_D, matrix_U);

    /*
        Eliminate lower left triangle.
    */
    // For each column,
    for (j = 0; j < N - 1; ++j) {
        block_j = decoder->Recovery[j]->Block;

        // For each row,
        for (i = j + 1; i < N; ++i) {
            block_i = decoder->Recovery[i]->Block;
            c_ij = *matrix_L++; // Matrix elements are stored column-first, top-down.

            gf256_muladd_mem(block_i, c_ij, block_j, decoder->Params.BlockBytes);
        }
    }

    /*
        Eliminate diagonal.
    */
    for (i = 0; i < N; ++i) {
        block = decoder->Recovery[i]->Block;

        decoder->Recovery[i]->Index = decoder->ErasuresIndices[i];

        gf256_div_mem(block, block, diag_D[i], decoder->Params.BlockBytes);
    }

    /*
        Eliminate upper right triangle.
    */
    for (j = N - 1; j >= 1; --j) {
        block_j = decoder->Recovery[j]->Block;

        for (i = j - 1; i >= 0; --i) {
            block_i = decoder->Recovery[i]->Block;
            c_ij = *matrix_U++; // Matrix elements are stored column-first, bottom-up.

            gf256_muladd_mem(block_i, c_ij, block_j, decoder->Params.BlockBytes);
        }
    }

    kfree(dynamicMatrix);
}

extern int cm256_decode(
    cm256_encoder_params params, // Encoder params
    cm256_block* blocks)         // Array of 'originalCount' blocks as described above
{
    CM256Decoder *state = kmalloc(sizeof(CM256Decoder), GFP_KERNEL);

    if (params.OriginalCount <= 0 || params.RecoveryCount <= 0 || params.BlockBytes <= 0) {
        return -1;
    }
    if (params.OriginalCount + params.RecoveryCount > 256) {
        return -2;
    }
    if (!blocks) {
        return -3;
    }

    // If there is only one block,
    if (params.OriginalCount == 1) {
        // It is the same block repeated
        blocks[0].Index = 0;
        return 0;
    }

    if (Initialize(state, params, blocks)) {
        return -5;
    }

    // If nothing is erased,
    if (state->RecoveryCount <= 0) {
        return 0;
    }

    // If m=1,
    if (params.RecoveryCount == 1) {
        DecodeM1(state);
        return 0;
    }

    // Decode for m>1
    Decode(state);
    return 0;
}
