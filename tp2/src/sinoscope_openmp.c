/*
 * sinoscope_openmp.c
 *
 *  Created on: 2011-10-14
 *      Author: francis
 *      {\__/}
 *      (o .o)
 *      (> .>)
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "sinoscope.h"
#include "color.h"
#include "util.h"

int sinoscope_image_openmp(sinoscope_t *ptr)
{
    //TODO("sinoscope_image_openmp");

    if (ptr == NULL)
        return -1;
     
    sinoscope_t sino = *ptr;
	int x, y, index, taylor;
	struct rgb c;
	float val, px, py;   
	
	
	#pragma omp parallel for private(x, y, px, py, val, taylor, index, c)
		for(x=1; x < sino.width-1;x++)
		{
				for(y = 1; y < sino.height-1; y++){
					px = sino.dx * y - 2 * M_PI;
					py = sino.dy * x - 2 * M_PI;
					val = 0.0f;
					
				for (taylor = 1; taylor <= sino.taylor; taylor += 2) {
					val += sin(px * taylor * sino.phase1 + sino.time) / taylor + cos(py * taylor * sino.phase0) / taylor;
				}
					
				val = (atan(1.0 * val) - atan(-1.0 * val)) / (M_PI);
				val = (val + 1) * 100;
				value_color(&c, val, sino.interval, sino.interval_inv);
				index = (y * 3) + (x * 3) * sino.width;
				sino.buf[index + 0] = c.r;
				sino.buf[index + 1] = c.g;
				sino.buf[index + 2] = c.b;
			}
		}
		
    return 0;
}
