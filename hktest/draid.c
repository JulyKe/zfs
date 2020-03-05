#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define MAX_GROUPSIZE 32
#define MAX_GROUPS 128
#define MAX_SPARES 100
#define MAX_DEVS (MAX_GROUPSIZE * MAX_GROUPS + MAX_SPARES)
#define MAX_ROWS 16384

#define EVAL_WORST 0
#define EVAL_MEAN 1
#define EVAL_RMS 2

typedef struct{
	int ndevs;
	int ngroups;
	int *groupsz;
	int nspares;
	int nrows;
	int **rows; /*each row maps all drives*/
	int nbroken; /*#broken drives*/
	int *broken; /*which drives are broken*/
} map_t;


typedef struct{
	int value;
	int order;
} pair_t;

//extern double eval_decluster(map_t *map, int how, int faults, int print);


static void permute_devs(int *in, int *out, int ndevs){
	pair_t tmp[ndevs];
	int i;
	int j;
	/* if just two devices, swap the order */
	if (ndevs == 2){
		i = in[0];
		j = in[1];
		out[0] = j;
		out[1] = i;
		return;
	}

	/* otherwise, assign random order */
	for (int i =0; i<ndevs; i++){
		tmp[i].value = in[i];
		tmp[i].order = mrand48();
		//printf("drive %d order %d \n", tmp[i].value, tmp[i].order);
	}

	/* sort drives according to the order */
	for (int i=0; i<ndevs; i++)
		for (int j = i+1; j<ndevs; j++)
			if (tmp[i].order > tmp[j].order){
				pair_t temp = tmp[i];
				tmp[i] = tmp[j];
				tmp[j] = temp;
			}

	/* store the sorted drives in out */
	for (int i=0; i<ndevs; i++){
		out[i] = tmp[i].value;
		//printf("-%d", out[i]);
	}
	//printf("\n");
}


static map_t * new_map(int ndevs, int ngroups, int nspares, int nrows){
	/* memory allocation for map_t */
	map_t *map = malloc(sizeof(map_t));
	map->ndevs = ndevs;
	map->ngroups = ngroups;
	map->nspares = nspares;
	map->nrows = nrows;

	/* memory allocation for map->groupsz */
	map->groupsz = malloc(sizeof(int) * ngroups);
	int groupsz = (ndevs - nspares) / ngroups;
	for (int i=0; i<ngroups; i++)
		map->groupsz[i] = groupsz;

	/* memory allocation for map->rows */
	map->rows = malloc(sizeof(int *) * nrows); /*?????*/
	for (int i=0; i<nrows; i++){
		map->rows[i] = malloc(sizeof(int)*ndevs);
		if (i==0)
		{
			for (int j=0; j<ndevs; j++)
				map->rows[i][j] = j;
		}
		else
		{
			permute_devs(map->rows[i-1], map->rows[i], ndevs);	
		}
	}


	/*memory allocation for map->brokens*/
	map->broken = malloc(sizeof(int)*nspares);
	map->nbroken = 0;
	return (map);
}

static map_t * develop_map(map_t *bmap){
	map_t *dmap = new_map(bmap->ndevs, bmap->ngroups, bmap->nspares, bmap->nrows * bmap->ndevs);
	//map_t *dmap = malloc(sizeof(int*) *bmap->nrows);
	for (int base=0; base < bmap->nrows; base++){
		printf("\n -------- base --------- \n");
		for (int add=0; add < bmap->ndevs; add++){
			for (int j=0; j < bmap->ndevs; j++){
				dmap->rows[base*bmap->ndevs+add][j] = (bmap->rows[base][j]+add) % bmap->ndevs;
				printf("-%d", dmap->rows[base*bmap->ndevs+add][j]);
			}
			printf("\n");
		}
	}
	return dmap;
}


static void free_map(map_t *map){
	free(map->broken);
	for (int i=0; i<map->nrows; i++)
		free(map->rows[i]);
	free(map->rows);
	free(map);
}


static int is_broken(map_t *map, int dev){
	for (int i=0; i<map->nbroken; i++)
		if (dev == map->broken[i])
			return 1;
	return 0;
}


static int eval_resilver(map_t *map, int print){
	/* extract the parameters from map */
	int ndevs = map->ndevs;
	int ngroups = map->ngroups;
	int nspares = map->nspares;
	int nrows = map->nrows;
	int reads[ndevs];
	int writes[ndevs];
	int max_read = 0;
	int max_write = 0;
	int max_ios = 0;


	/*fill a block of memory with a particular value 0.*/
	memset(reads, 0, sizeof(int) * ndevs);
	memset(writes, 0, sizeof(int) * ndevs);


	/* resilver all rows*/
	for (int i=0; i<nrows; i++){
		int *each_row = map->rows[i];	
		/* resilver all groups with broken drives */	
		int index = 0;
		for (int j=0; j<ngroups; j++){
			int fix = 0;
			int groupsz = map->groupsz[j];
			for (int k=0; k<groupsz && !fix; k++){
				fix = is_broken(map, each_row[index + k]);
			}
			/* if no failure in this group */
			if(!fix){
				index += groupsz;
				continue;
			}
			/* if there is failure in this group */
			int spareIndex = ndevs - nspares;
			printf("\nrow %d, group %d ", i, j);
			for (int k=0; k<groupsz; k++){
				int dev = each_row[index+k];
				if(!is_broken(map, dev)){
					printf("\n  -----reads----- %d", dev);
					reads[dev]++;	
				}else{
					assert(spareIndex < ndevs);
					while(is_broken(map, each_row[spareIndex])){
						spareIndex++;	
						assert(spareIndex < ndevs);
					}
					printf("\n  ----writes----- %d", each_row[spareIndex]);
					writes[each_row[spareIndex++]]++;
				}	
			}
			index += groupsz;
		}
	}

	/* find the drive with most I/O */
	for (int i=0; i<ndevs; i++){
		if (reads[i] > max_read){
			max_read = reads[i];	
		}
		if (writes[i] > max_write){
			max_write = writes[i];	
		}
		if (reads[i] + writes[i] > max_ios){
			max_ios = reads[i]+writes[i];	
		}
	}

	/* print the reads and writes out */
	if (print){
		printf("\nReads: ");	
		for (int i=0; i<ndevs; i++){
			printf("%5.3f", (double) reads[i]*ngroups/nrows);
		}
		printf("\nWrites: ");	
		for (int i=0; i<ndevs; i++){
			printf("%5.3f", (double) writes[i]*ngroups/nrows);
		}
	}
	return max_ios;	
}


static double eval_decluster(map_t *map, int how, int faults, int print){
	int ios;
	int worst1 = -1;
	int worst2 = -1;
	int sum = 0;
	int sumsq = 0;
	int max_ios = 0;
	double val = 0;
	int iter_num = 0;

	/*try to inject failure in each drive*/
	map->nbroken = faults;
	for (int f1=0; f1 < map->ndevs; f1++){
		map->broken[0] = f1;
		printf("\n >>>>>>>>>>faults>>>>>>>>> %d\n", f1);
		if (faults <2){
			/* evaluate the single failure*/	
			ios = eval_resilver(map, 1);
			iter_num++;
			sum += ios;
			sumsq += ios*ios;
			if (max_ios < ios){
				max_ios = ios;
				worst1 = f1;	
			}
		
		}else{
			for (int f2=f1+1; f2 < map->ndevs; f2++){
				/* evaluate the double failures*/	
				map->broken[1] = f2;
				ios = eval_resilver(map, 1);	
				iter_num++;
				sum += ios;
				sumsq += ios*ios;
				if (max_ios < ios){
					max_ios = ios;
					worst1 = f1;
					worst2 = f2;
				}
			}	
		
		}
	}
	map->nbroken = 0;
	switch(how){
		case EVAL_WORST:
			val = max_ios;
			break;
		case EVAL_MEAN:
			val=((double)sum)/iter_num;	
			break;
		case EVAL_RMS: 
			val = sqrt(((double)sumsq)/iter_num);
			break;
		default:
			assert(0);
	}
	return (val/map->nrows)*map->ngroups;
}


static int draid_permutation_generate(){
	int ndevs = 12;
	int ngroups = 2;
	int nspares = 2;
	int nrows = 100;
	printf("ndevs %d, ngroups %d, nspares %d, nrows %d \n", ndevs, ngroups, nspares, nrows);

	/* create the base map for permutation */
	map_t *bmap = new_map(ndevs, ngroups, nspares, nrows);
	double val = eval_decluster(bmap, EVAL_MEAN, 1, 1);
	printf("\n********Result*********%f", val);

	/* create the develop map for permutation */
	map_t *dmap = develop_map(bmap);

	/* free the base and develop map memory */
	free_map(bmap);
	free_map(dmap);

	return 0;
}


int main(int argc, int *argv[]){
	printf("Hello World!\n");
	draid_permutation_generate();
	return 0;
}
