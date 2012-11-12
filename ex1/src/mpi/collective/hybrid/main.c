/* -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
 * File Name : main.c
 * Creation Date : 30-10-2012
 * Last Modified : Mon 12 Nov 2012 10:03:37 AM EET
 * Created By : Greg Liras <gregliras@gmail.com>
 * Created By : Alex Maurogiannis <nalfemp@gmail.com>
 _._._._._._._._._._._._._._._._._._._._._.*/

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#include "common.h"

#define BLOCK_ROWS 1


void process_rows(int k, int rank, int N, int max_rank, double *A)
{
    /*      performs the calculations for a given set of rows.
     *      In this hybrid version each thread is assigned blocks of 
     *      continuous rows in a cyclic manner.
     */
    int i, j, w;
    double l;
    /* For every cyclic repetition of a block */
    for (i = (rank + ((BLOCK_ROWS * max_rank) * (k / (BLOCK_ROWS * max_rank)))); i < N ; i+=(max_rank * BLOCK_ROWS)) {
        if (i > k) {
            /* Calculate each continuous row in the block*/
            for (w = i; w < (i + BLOCK_ROWS) && w < (N * N); w++){
                l = A[(w * N) + k] / A[(k * N) + k];
                for (j = k; j < N; j++) {
                    A[(w * N) + j] = A[(w * N) + j] - l * A[(k* N) + j];
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    int k;
    int N;
    int rank;
    int max_rank;
    int last_rank;
    double *A = NULL;
    double sec = 0;

    int ret = 0;
    FILE *fp = NULL;
    usage(argc, argv);

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &max_rank);

    if (rank == 0) {
        debug("rank: %d opens file: %s\n", rank, argv[1]);
        fp = fopen(argv[1], "r");
        if(fp) {
            if(!fscanf(fp, "%d\n", &N)) {
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }
        else {
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

    }

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Bcast(&N, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* Everyone allocates the whole table */
    debug("Max rank = %d\n", max_rank);
    if((A = allocate_2d_with_padding(N, N, max_rank)) == NULL) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    /* Root Parses file */
    if (rank == 0) {
        if(parse_matrix_2d(fp, N, N, A) == NULL) {
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        fclose(fp);
        fp = NULL;
    }
    /* And distributes the table */
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Bcast(A, N*N, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    last_rank = (N - 1) % max_rank;
 
    if(rank == 0) {
        sec = timer();
    }

    for (k = 0; k < N - 1; k++) {
        /* The owner of the row for this k broadcasts it*/
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Bcast(&A[k * N], N, MPI_DOUBLE, ((k % (max_rank * BLOCK_ROWS)) / BLOCK_ROWS), MPI_COMM_WORLD);

        process_rows(k, rank, N, max_rank, A);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
        sec = timer();
        printf("Calc Time: %lf\n", sec);
    }
    ret = MPI_Finalize();

    if(ret == 0) {
        debug("%d FINALIZED!!! with code: %d\n", rank, ret);
    }
    else {
        debug("%d NOT FINALIZED!!! with code: %d\n", rank, ret);
    }

    /* Last process has table */
    if (rank == last_rank) {
        //print_matrix_2d(N, N, A);
        fp = fopen(argv[2], "w");
        fprint_matrix_2d(fp, N, N, A);
        fclose(fp);
    }
    free(A);

    return 0;
}