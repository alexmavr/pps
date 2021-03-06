/* -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
 * File Name : main.c
 * Creation Date : 30-10-2012
 * Last Modified : Thu 29 Nov 2012 05:59:32 PM EET
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

void process_rows(int k, int rank, int N, int workload, int max_rank, \
        double **Ap2D, double *Ak)
{
    /*      performs the calculations for a given set of rows.
     *      In this hybrid version each thread is assigned blocks of 
     *      continuous rows in a cyclic manner.
     */
    int j, w;
    double l;
    int start;

    start = k / max_rank;
    /* If you have broadcasted, dont do calculations for that row */
    if (rank <= (k % max_rank)){
        start++;
    }
    for (w = start; w < workload; w++) {
        l = Ap2D[w][k] / Ak[k];
        for (j = k; j < N; j++) {
            Ap2D[w][j] = Ap2D[w][j] - l * Ak[j];
        }
    }
}

int main(int argc, char **argv)
{
    int k;
    int i;
    int N;
    int rank;
    int max_rank;
    int bcaster;
    int workload;
    int ret = 0;
    double *A = NULL;
    double **A2D = NULL;
    double *Ap = NULL;
    double **Ap2D = NULL;
    double *Ak = NULL;
    double sec = 0;
    FILE *fp = NULL;
    void (*propagate) (void*, int, MPI_Datatype, int, MPI_Comm);
    time_struct ts;

    usage(argc, argv);
    propagate = get_propagation(argc, argv);

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &max_rank);

    /* Root gets the matrix */
    if (rank == 0) {
        Matrix *mat = get_matrix(argv[1], max_rank, CYCLIC);
        N = mat->N;
        A = mat->A;
        A2D = appoint_2D(A, N+max_rank, N);
    }

    /* And broadcasts N */
    MPI_Bcast(&N, 1, MPI_INT, 0, MPI_COMM_WORLD);

    workload = (N / max_rank) + 1;

    /* Allocations */
    Ak = malloc(N * sizeof(double));
    Ap = malloc(workload * N * sizeof(double));

    MPI_Scatter(A, workload * N, MPI_DOUBLE, \
            Ap, workload * N, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    Ap2D = appoint_2D(Ap, workload, N);

    /* Init Communication Timer */
    time_struct_init(&ts);

    /* Start Total Timer */
    MPI_Barrier(MPI_COMM_WORLD);
    if(rank == 0) {
        sec = timer();
    }

    for (k = 0; k < N - 1; k++) {
        /* Find who owns the k-th row */
        bcaster = k % max_rank;

        /* The broadcaster puts what's needed of his k-th
         * row in the Ak buffer */
        if (rank == bcaster) {
            i = k / max_rank;
            memcpy(&Ak[k], &Ap2D[i][k], (N-k) * sizeof(double));
        }

        /* Everyone receives the k-th row */
        time_struct_set_timestamp(&ts);
        (*propagate) (&Ak[k], N-k, MPI_DOUBLE, bcaster, MPI_COMM_WORLD);
        time_struct_add_timestamp(&ts);

        /* And off you go to work. */
        process_rows(k, rank, N, workload, max_rank, Ap2D, Ak);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
        sec = timer();
        printf("Calc Time: %lf\n", sec);
    }

    printf("Rank: %d Comm Time: %lf\n", rank, get_seconds(&ts));

    /* Gather the table from each thread's Ap */
    gather_to_root_cyclic(Ap2D, max_rank, rank, 0, A2D, N, N);

    ret = MPI_Finalize();
    if(ret == 0) {
        debug("%d FINALIZED!!! with code: %d\n", rank, ret);
    }
    else {
        debug("%d NOT FINALIZED!!! with code: %d\n", rank, ret);
    }

    if (rank == 0) {
        upper_triangularize(N, A2D);
        fp = fopen(argv[2], "w");
        fprint_matrix_2d(fp, N, N, A);
        fclose(fp);
        free(A);
        free(A2D);
    }
    free(Ap);
    free(Ap2D);
    free(Ak);
    return 0;
}
