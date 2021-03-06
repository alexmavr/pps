// vim: set syntax=opencl:
/*
 *  dmv_main.cl -- DMV front-end program.
 *
 *  Copyright (C) 2010-2012, Computing Systems Laboratory (CSLab)
 *  Copyright (C) 2010-2012, Vasileios Karakasis
 */ 

#include <stdlib.h>
#include <stdio.h>
#include <CL/opencl.h>
#include "alloc.h"
#include "dmv.h"
#include "error.h"
#include "timer.h"

#ifndef VALUES_MAX
#   define VALUES_MAX MAKE_VALUE_CONSTANT(1.)
#endif

#ifndef EPS
#   define EPS MAKE_VALUE_CONSTANT(1.e-6)
#endif

#ifndef NR_ITER
#   define NR_ITER 100
#endif

#define MAX_SOURCE_SIZE (0x100000)

static inline size_t min(size_t a, size_t b)
{
    return a < b ? a : b;
}

static inline size_t max(size_t a, size_t b)
{
    return a > b ? a : b;
}

static void transpose(value_t ** A, uint n) {
    uint i,j;
    value_t temp;
    for (i=0; i<n; i++)
        for (j=i+1; j<n; j++) {
            temp = A[i][j];
            A[i][j] = A[j][i];
            A[j][i] = temp;
        }
}

static void check_result(const value_t *test, const value_t *orig, size_t n)
{
    printf("Checking ... ");
    size_t  i_fail = vec_equals(test, orig, n, EPS);
    if (!i_fail) {
        printf("PASSED\n");
    } else {
        printf("FAILED (index: %ld)\n", i_fail - 1);
        printf("%" VALUE_FORMAT " != " "%" VALUE_FORMAT "\n",
                test[i_fail-1], orig[i_fail-1]);
    }
}

static void report_results(xtimer_t *timer, size_t n)
{
    double  elapsed_time = timer_elapsed_time(timer);
    size_t  flops        = 2*n*n*NR_ITER;

    printf("Elapsed time: %lf s\n", elapsed_time);
    printf("Performance:  %lf Gflop/s\n", flops*1.e-9 / elapsed_time);
}

static void print_usage()
{
    printf("Usage: [GPU_KERNEL=<kernel_no>] [GPU_BLOCK_SIZE=<size>] "
            "%s <matrix size>\n", program_name);
    printf("GPU_KERNEL defaults to 0\n");
    printf("GPU_BLOCK_SIZE defaults to 256\n");
    printf("Available kernels [id:name]:\n");
    size_t i;
    for (i = 0; i < GPU_KERNEL_END; ++i) {
        printf("\t%zd:%s\n", i, gpu_kernels[i].name);
    }
}

int main(int argc, char **argv)
{
    set_program_name(argv[0]);
    if (argc < 2) {
        warning(0, "too few arguments");
        print_usage();
        exit(EXIT_FAILURE);
    }

    uint n = (uint) atoi(argv[1]);
    if (!n)
        error(0, "invalid argument: %s", argv[1]);

    /* Read block size and kernel to launch from the environment */
    const char *env_gpu_kernel = getenv("GPU_KERNEL");
    const char *env_gpu_block_size = getenv("GPU_BLOCK_SIZE");
    int kern = (env_gpu_kernel) ? atoi(env_gpu_kernel) : GPU_NAIVE;
    int block_size = (env_gpu_block_size) ? atoi(env_gpu_block_size) : 256;
    size_t orig_n = n;  // original matrix size

    printf("Matrix size: %zd\n", orig_n);
    printf("Adjusted matrix size: %u\n", n);

    /*
     * Allocate the structures.
     */
    value_t **A = (value_t **) calloc_2d(n, n, sizeof(**A));
    if (!A)
        error(1, "alloc_2d failed");

    value_t *x = (value_t *) calloc(n, sizeof(*x));
    if (!x)
        error(1, "malloc failed");

    value_t *y_serial = (value_t *) calloc(n, sizeof(*y_serial));
    if (!y_serial)
        error(1, "malloc failed");

    value_t *y = (value_t *) calloc(n, sizeof(*y));
    if (!y)
        error(1, "malloc failed");

    /* Initialize */
    srand48(0);
    mat_init_rand(A, orig_n, VALUES_MAX);
    vec_init_rand(x, orig_n, VALUES_MAX);
    vec_init(y_serial, orig_n, MAKE_VALUE_CONSTANT(0.0));
    vec_init(y, orig_n, MAKE_VALUE_CONSTANT(0.0));

    /* Setup timers */
    xtimer_t timer;

    /* Compute serial */
#ifdef SERIAL_KERNEL
    printf(">>>> Begin of record <<<<\n");
    printf("Serial version:\n");
    timer_clear(&timer);
    timer_start(&timer);
    for (size_t i = 0; i < NR_ITER; ++i)
        dmv_serial(A, x, y_serial, orig_n);
    timer_stop(&timer);
    report_results(&timer, orig_n);
    printf(">>>> End of record <<<<\n");
#endif  // SERIAL_KERNEL

#ifdef OPENMP_KERNEL
    /* Compute OpenMP */
    printf(">>>> Begin of record <<<<\n");
    printf("OpenMP version:\n");
    timer_clear(&timer);
    timer_start(&timer);
    for (size_t i = 0; i < NR_ITER; ++i)
        dmv_omp(A, x, y, orig_n);
    timer_stop(&timer);
#ifndef _NOCHECK_
    check_result(y, y_serial, orig_n);
#endif
    report_results(&timer, orig_n);
    printf(">>>> End of record <<<<\n");
#endif  // OPENMP_KERNEL

#ifdef GPU_KERNEL
    cl_int errv = 0;
    cl_context context;
    cl_command_queue queue;
    cl_device_id device=NULL;
    cl_platform_id platform_id = NULL;
    cl_uint ret_num_devices;
    cl_uint ret_num_platforms;
    cl_program program = NULL;
    cl_kernel kernel = NULL;


    FILE *fp;
    char fileName[] = "./dmv_kernels.cl";
    char *source_str;
    size_t source_size;

    /* Initialization Begin */
    // Platform
    errv = clGetPlatformIDs(1, &platform_id, &ret_num_platforms);
    if (errv != CL_SUCCESS) {
        printf("Error getting platform id\n");
        cl_error(errv);
        exit(errv);
    }
    // Device
    errv = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_GPU, 1, 
            &device, &ret_num_devices);
    if (errv != CL_SUCCESS) {
        printf("Error getting device ids\n");
        cl_error(errv);
        exit(errv);
    }
    // Context
    context = clCreateContext(0, 1, &device, NULL, NULL, &errv);
    if (errv != CL_SUCCESS) {
        printf("Error creating context\n");
        cl_error(errv);
        exit(errv);
    }
    // Command-queue
    queue = clCreateCommandQueue(context, device, 0, &errv);
    if (errv != CL_SUCCESS) {
        printf("Error creating command queue \n");
        cl_error(errv);
        exit(errv);
    }
    /* Initialization Complete */


    printf(">>>> Begin of record <<<<\n");

    /* Calculate available Shared Memory */
    cl_ulong shmem_size;
    clGetDeviceInfo(device, CL_DEVICE_LOCAL_MEM_SIZE,
                                sizeof(shmem_size), &shmem_size, NULL);
    printf("Shared memory size: %ld bytes or %lu elements \n", shmem_size,
                                shmem_size/sizeof(value_t));

    /* Calculate grid and block sizes */
    const size_t local_ws = min(block_size, CL_DEVICE_MAX_WORK_ITEM_SIZES);
    size_t global_ws = n < local_ws ? 1 : n / local_ws;  // is at least 1 block
    if ((n % local_ws != 0) && (n > local_ws)) {
        global_ws++;
    }
    global_ws *= local_ws; 

    printf("\n");
    printf("N: %u\n",n);
    printf("Global work-items: %lu\n", global_ws);
    printf("Local work-items: %lu\n", local_ws);
    printf("\n");

    /* GPU allocations */
    if (kern == 2) {
        // transpose A if using the shared mem kernel
        transpose(A, n);
    }
    cl_mem gpu_A = clCreateBuffer(context, CL_MEM_READ_ONLY | \
                    CL_MEM_USE_HOST_PTR , n * n * sizeof(value_t), *A, &errv);
    if (!gpu_A) {
        cl_error(errv);
        error(0, "A: gpu_alloc failed: %d", errv);
    }

    cl_mem gpu_x = clCreateBuffer(context, CL_MEM_READ_ONLY | \
                    CL_MEM_ALLOC_HOST_PTR, n * sizeof(value_t), x, &errv);
    if (!gpu_x) {
        cl_error(errv);
        error(0, "x: gpu_alloc failed: %d", errv);
    }

    vec_init(y, n, MAKE_VALUE_CONSTANT(0.0));
    cl_mem gpu_y = clCreateBuffer(context, CL_MEM_WRITE_ONLY, \
                    n * sizeof(value_t), y, &errv);
    if (!gpu_y) {
        cl_error(errv);
        error(0, "y: gpu_alloc failed: %d", errv);
    }

    if (kern >= GPU_KERNEL_END) {
        cl_error(errv);
        error(0, "the requested kernel does not exist");
    }

    printf("GPU kernel version: %s\n", gpu_kernels[kern].name);

    /* Load the source code containing the kernels*/
    fp = fopen(fileName, "r");
    if (!fp) {
        fprintf(stderr, "Failed to load kernel.\n");
        exit(1);
    }
    source_str = (char*)malloc(MAX_SOURCE_SIZE);
    source_size = fread(source_str, 1, MAX_SOURCE_SIZE, fp);
    fclose(fp);

    /* Create Program from the source */
    program = clCreateProgramWithSource(context, 1, \
            (const char **)&source_str, (const size_t *)&source_size, &errv);
    if (!program)
        error(0, "Could not read program: %s", errv);

    /* Build Kernel Program */
    errv = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (errv != CL_SUCCESS) {
        printf("Error building kernel: %d\n\n", errv);

        /* Generate Build Log on Failure*/
        char * build_log;
        size_t log_size;
        errv = clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, \
                0, NULL, &log_size);
        build_log = (char *) malloc((log_size+1) * sizeof(char));
        errv |= clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, \
                log_size, build_log, NULL);
        if (errv != CL_SUCCESS) {
            printf("Error generating program build log\n");
            cl_error(errv);
            exit(errv);
        }
        build_log[log_size]= '\0';
        printf("%s\n\n", build_log);
        free(build_log);
    }


    /* Create OpenCL Kernel */
    kernel = clCreateKernel(program, gpu_kernels[kern].name, &errv);
    if (errv != CL_SUCCESS) {
        printf("Error creating Kernel \n");
        cl_error(errv);
        exit(errv);
    }

    /* Set the Kernel Arguments */
    errv = clSetKernelArg(kernel, 0, sizeof(cl_mem), &gpu_A);
    errv |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &gpu_x);
    errv |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &gpu_y);
    errv |= clSetKernelArg(kernel, 3, sizeof(uint), &n);

    if (kern == 1) {
        /* Memory Coalesced Kernel */
        errv |= clSetKernelArg(kernel, 4, local_ws * sizeof(value_t), NULL);
    }
    if (kern == 2) {
        /* Shared Memory Kernel */

        // Subtract products and x buffers from available local memory
        shmem_size -= 2 * local_ws * sizeof(value_t);

        // Prefetch local_ws elements, or as many the local mem can fit
        cl_uint w;
        w = min(local_ws, (shmem_size / sizeof(value_t)));

        printf("Will prefetch x in chunks of %u\n", w);

        errv |= clSetKernelArg(kernel, 4, sizeof(cl_uint), &w);
        errv |= clSetKernelArg(kernel, 5, local_ws * sizeof(value_t), NULL);
        errv |= clSetKernelArg(kernel, 6, w*sizeof(value_t), NULL);
    }
    if (errv != CL_SUCCESS) {
        printf("Error setting Kernel arguments %d\n", errv);
        cl_error(errv);
        exit(errv);
    }

    errv = clEnqueueWriteBuffer(queue, gpu_A, CL_TRUE, 0, \
                    sizeof(value_t) * n * n, (void *) *A, 0, NULL, NULL);
    errv |= clEnqueueWriteBuffer(queue, gpu_x, CL_TRUE, 0, \
                    sizeof(value_t) * n, (void *)x, 0, NULL, NULL);
    if (errv != CL_SUCCESS) {
        printf("Error enqueuing write buffers: %d\n", errv);
        cl_error(errv);
        exit(errv);
    }
    
    /* Start Timing and Enqueue the kernel */
    timer_clear(&timer);
    errv = clFinish(queue);
    timer_start(&timer);
    for (size_t i = 0; i < NR_ITER; ++i) {
        errv |= clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_ws, \
                &local_ws, 0, NULL, NULL);
    }

    /* Wait for all queue items to finish and stop timing */
    errv = clFinish(queue);
    timer_stop(&timer);

    if (errv != CL_SUCCESS) {
        printf("Error during kernel queuing and execution: %d\n", errv);
        cl_error(errv);
        exit(errv);
    }

    /* Read the Result */
    errv = clEnqueueReadBuffer(queue, gpu_y, CL_TRUE, 0, \
                n * sizeof(value_t), y, 0, NULL, NULL);
    if (errv != CL_SUCCESS) {
        printf("Error enqueuing read buffer\n");
        cl_error(errv);
        exit(errv);
    }
    errv = clFlush(queue);
    if (errv != CL_SUCCESS) {
        printf("Error flushing queue\n");
        cl_error(errv);
        exit(errv);
    }

#ifndef _NOCHECK_
    check_result(y, y_serial, orig_n);
#endif

    report_results(&timer, orig_n);
    printf(">>>> End of record <<<<\n");

    /* Cleanup */
    errv = clFinish(queue);
    errv = clReleaseKernel(kernel);
    errv = clReleaseProgram(program);
    errv = clReleaseMemObject(gpu_A);
    errv = clReleaseMemObject(gpu_x);
    errv = clReleaseMemObject(gpu_y);
    errv = clReleaseCommandQueue(queue);
    errv = clReleaseContext(context);

#endif  // GPU_KERNEL 
    /* Free resources on host */
    free_2d((void **) A);
    free(x);
    free(y);
    free(y_serial);

    return EXIT_SUCCESS;
}
