/* ------------------------------------------------------------------------
 *
 * geqo_recombination.cpp
 *	 misc recombination procedures
 *
 * IDENTIFICATION
 * src/gausskernel/optimizer/geqo/geqo_recombination.cpp
 *
 * -------------------------------------------------------------------------
 */
/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */
/* -- parts of this are adapted from D. Whitley's Genitor algorithm -- */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"

/*
 * init_tour
 *
 *	 Randomly generates a legal "traveling salesman" tour
 *	 (i.e. where each point is visited only once.)
 *	 Essentially, this routine fills an array with all possible
 *	 points on the tour and randomly chooses the 'next' city from
 *	 this array.  When a city is chosen, the array is shortened
 *	 and the procedure repeated.
 */
void init_tour(PlannerInfo* root, Gene* tour, int num_gene)
{
    Gene* tmp = NULL;
    int remainder;
    int next, i;

    /* Fill a temp array with the IDs of all not-yet-visited cities */
    tmp = (Gene*)palloc(num_gene * sizeof(Gene));

    for (i = 0; i < num_gene; i++)
        tmp[i] = (Gene)(i + 1);

    remainder = num_gene - 1;

    for (i = 0; i < num_gene; i++) {
        /* choose value between 0 and remainder inclusive */
        next = geqo_randint(root, remainder, 0);
        /* output that element of the tmp array */
        tour[i] = tmp[next];
        /* and delete it */
        tmp[next] = tmp[remainder];
        remainder--;
    }

    pfree_ext(tmp);
}

/* alloc_city_table
 *
 *	 allocate memory for city table
 */
City* alloc_city_table(PlannerInfo* root, int num_gene)
{
    City* city_table = NULL;

    /*
     * palloc one extra location so that nodes numbered 1..n can be indexed
     * directly; 0 will not be used
     */
    city_table = (City*)palloc((num_gene + 1) * sizeof(City));

    return city_table;
}

/* free_city_table
 *
 *	  deallocate memory of city table
 */
void free_city_table(PlannerInfo* root, City* city_table)
{
    pfree_ext(city_table);
}
