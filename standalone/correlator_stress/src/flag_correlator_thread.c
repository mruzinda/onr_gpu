/* flag_correlator_thread.c
 * 
 * Routine to correlate received packets
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <xgpu.h>
#include "hashpipe.h"
#include "flag_databuf.h"

#define ELAPSED_NS(start, stop) \
  (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000*1000+(stop.tv_nsec-start.tv_nsec))


// Create thread status buffer
static hashpipe_status_t * st_p;


// Run method for the thread
// It is meant to do the following:
//     (1) Initialize status buffer
//     (2) Start main loop
//         (2a) Wait for input buffer block to be filled
//         (2b) Print out some data in the block
static void * run(hashpipe_thread_args_t * args) {
    // Local aliases to shorten access to args fields
    flag_gpu_input_databuf_t * db_in = (flag_gpu_input_databuf_t *)args->ibuf;
    flag_correlator_output_databuf_t * db_out = (flag_correlator_output_databuf_t *)args->obuf;
    hashpipe_status_t st = args->st;
    const char * status_key = args->thread_desc->skey;

    st_p = &st; // allow global (this source file) access to the status buffer

    // Initialize correlator integrator status to "off"
    // Initialize starting mcnt to 0 (INTSYNC)
    char integ_status[17];
    int gpu_dev = 0;
    hashpipe_status_lock_safe(&st);
    hputs(st.buf, "INTSTAT", "off");
    hputi8(st.buf, "INTSYNC", 0);
    hputi4(st.buf, "INTCOUNT", 40);
    hgeti4(st.buf, "GPUDEV", &gpu_dev);
    hputi4(st.buf, "GPUDEV", gpu_dev);
    hashpipe_status_unlock_safe(&st);

    // Initialize xGPU context structure
    // Comment from PAPER:
    //   Initialize context to point at first input and output memory blocks.
    //   This seems redundant since we do this just before calling
    //   xgpuCudaXengine, but we need to pass something in for array_h and
    //   matrix_x to prevent xgpuInit from allocating memory.
    XGPUContext context;
    context.array_h  = (ComplexInput *)db_in->block[0].data;
    context.matrix_h = (Complex *)db_out->block[0].data;
   
    context.array_len = (db_in->header.n_block * sizeof(flag_gpu_input_block_t) - sizeof(flag_gpu_input_header_t))/sizeof(ComplexInput);
    context.matrix_len = (db_out->header.n_block * sizeof(flag_correlator_output_block_t) - sizeof(flag_correlator_output_header_t))/sizeof(Complex);

    int xgpu_error = xgpuInit(&context, gpu_dev);
    if (XGPU_OK != xgpu_error) {
        fprintf(stderr, "ERROR: xGPU initialization failed (error code %d)\n", xgpu_error);
        return THREAD_ERROR;
    }
    

    // Mark thread as ready to run
    hashpipe_status_lock_safe(&st);
    hputi4(st.buf, "CORREADY", 1);
    hashpipe_status_unlock_safe(&st);
 
    int rv;
    int curblock_in = 0;
    int curblock_out = 0;
    uint64_t start_mcnt = 0;
    uint64_t last_mcnt = 0;
    int int_count; // Number of blocks to integrate per dump
    struct timespec start, stop;
    uint64_t elapsed_ns = 0;
    int num_blocks = 0;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (run_threads()) {
        
        // Wait for input buffer block to be filled
        while ((rv=flag_gpu_input_databuf_wait_filled(db_in, curblock_in)) != HASHPIPE_OK) {
            if (rv==HASHPIPE_TIMEOUT) {
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, status_key, "waiting for free block");
                hashpipe_status_unlock_safe(&st);
            }
            else {
                hashpipe_error(__FUNCTION__, "error waiting for filled databuf block");
                pthread_exit(NULL);
                break;
            }
        }
      
        // Retrieve correlator integrator status
        hashpipe_status_lock_safe(&st);
        hgets(st.buf, "INTSTAT", 16, integ_status);
        hashpipe_status_unlock_safe(&st);

        // Get header information for this block 
        flag_gpu_input_header_t tmp_header;
        memcpy(&tmp_header, &db_in->block[curblock_in].header, sizeof(flag_gpu_input_header_t));

        // If the correlator integrator status is "off,"
        // Free the input block and continue
        if (strcmp(integ_status, "off") == 0) {
            fprintf(stderr, "COR: Correlator is off...\n");
            flag_gpu_input_databuf_set_free(db_in, curblock_in);
            curblock_in = (curblock_in + 1) % db_in->header.n_block;
            continue;
        }

        // If the correlator integrator status is "start,"
        // Get the correlator started
        // The INTSTAT string is set to "start" by the net thread once it's up and running
        if (strcmp(integ_status, "start") == 0) {
           hashpipe_status_lock_safe(&st);
           hgeti4(st.buf, "NETMCNT", (int *)(&start_mcnt));
           hashpipe_status_unlock_safe(&st); 
            // Check to see if block's starting mcnt matches INTSYNC
            if (db_in->block[curblock_in].header.mcnt < start_mcnt) {
                fprintf(stderr, "COR: Unable to start yet...\n");
                // starting mcnt not yet reached
                // free block and continue
                flag_gpu_input_databuf_set_free(db_in, curblock_in);
                curblock_in = (curblock_in + 1) % db_in->header.n_block;
                continue;
            } else if (db_in->block[curblock_in].header.mcnt == start_mcnt) {
                // set correlator integrator to "on"
                fprintf(stderr, "COR: Starting correlator!\n");
                strcpy(integ_status, "on");
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, "INTSTAT", integ_status);
                hgeti4(st.buf, "INTCOUNT", &int_count);
                hashpipe_status_unlock_safe(&st);

                // Compute last mcount
                last_mcnt = start_mcnt + int_count*Nm - 1;
            } else {
                fprintf(stderr, "COR: We missed the start of the integration!\n");
                // we apparently missed the start of the integation... ouch...
            }
        }

        // If we get here, then integ_status == "on" or "stop"
        num_blocks++;

        // Setup for current chunk
        context.input_offset  = curblock_in  * sizeof(flag_gpu_input_block_t) / sizeof(ComplexInput);
        context.output_offset = curblock_out * sizeof(flag_correlator_output_block_t) / sizeof(Complex);
        
        int doDump = 0;
        if ((db_in->block[curblock_in].header.mcnt + int_count*Nm - 1) >= last_mcnt) {
            doDump = 1;

            // Wait for new output block to be free
            while ((rv=flag_correlator_output_databuf_wait_free(db_out, curblock_out)) != HASHPIPE_OK) {
                if (rv==HASHPIPE_TIMEOUT) {
                    continue;
                } else {
                    hashpipe_error(__FUNCTION__, "error waiting for free databuf");
                    fprintf(stderr, "rv = %d\n", rv);
                    pthread_exit(NULL);
                    break;
                }
            }

        }

        xgpuCudaXengine(&context, doDump ? SYNCOP_DUMP : SYNCOP_SYNC_TRANSFER);
        
        if (doDump) {
            xgpuClearDeviceIntegrationBuffer(&context);
            //xgpuReorderMatrix((Complex *)db_out->block[curblock_out].data);
            db_out->block[curblock_out].header.mcnt = start_mcnt;
            
            // Mark output block as full and advance
            flag_correlator_output_databuf_set_filled(db_out, curblock_out);
            curblock_out = (curblock_out + 1) % db_out->header.n_block;
            start_mcnt = last_mcnt + 1;
            last_mcnt = start_mcnt + int_count*Nm -1;
        }

        flag_gpu_input_databuf_set_free(db_in, curblock_in);
        curblock_in = (curblock_in + 1) % db_in->header.n_block;

        clock_gettime(CLOCK_MONOTONIC, &stop);
        elapsed_ns = ELAPSED_NS(start, stop);
        if (num_blocks % 500 == 0) {
            fprintf(stderr, "COR: Elapsed time (ns): %lld\n", (long long int)(elapsed_ns/num_blocks));
            fprintf(stderr, "COR: Theoretical data rate (Gbps): %f\n", (N_BYTES_PER_BLOCK*8.0)/(elapsed_ns/num_blocks));
        }
        pthread_testcancel();
        
    }

    // Thread terminates after loop
    return NULL;
}



// Thread description
static hashpipe_thread_desc_t x_thread = {
    name: "flag_correlator_thread",
    skey: "CORSTAT",
    init: NULL,
    run:  run,
    ibuf_desc: {flag_gpu_input_databuf_create},
    obuf_desc: {flag_correlator_output_databuf_create}
};

static __attribute__((constructor)) void ctor() {
    register_hashpipe_thread(&x_thread);
}

