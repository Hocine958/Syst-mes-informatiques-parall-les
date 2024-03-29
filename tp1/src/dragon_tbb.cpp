/*
 i* dragon_tbb.c
 *
 *  Created on: 2011-08-17
 *      Author: Francis Giraldeau <francis.giraldeau@gmail.com>
 */

#include <iostream>
#include <atomic>
#include <stdarg.h>
#include <string.h>

extern "C" {
#include "dragon.h"
#include "color.h"
#include "utils.h"
}
#include "dragon_tbb.h"
#include "tbb/tbb.h"
#include "TidMap.h"

using namespace std;
using namespace tbb;

std::atomic<int> interval;

pthread_mutex_t mutex_stdout;
void printf_threadsafe(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	pthread_mutex_lock(&mutex_stdout);
	vprintf(format, list);
	pthread_mutex_unlock(&mutex_stdout);
	va_end(list);
}

class DragonLimits {
	public:
		piece_t pieces[NB_TILES];
		
		DragonLimits()
		{
			for (size_t i = 0; i < NB_TILES; i++)
			{
				piece_init(&pieces[i]);
				pieces[i].orientation = tiles_orientation[i];
			}
		}

		DragonLimits(DragonLimits &dragon, split)
		{
			for (size_t i = 0; i < NB_TILES; i++)
			{
				piece_init(&pieces[i]);
				pieces[i].orientation = tiles_orientation[i];
			}
		}

		void join(DragonLimits &dragon)
		{
			for (size_t i = 0; i < NB_TILES; i++)
				piece_merge(&pieces[i], dragon.pieces[i], tiles_orientation[i]);
		}
		
		void operator()(const blocked_range<uint64_t> &range)
		{
			for (size_t i = 0; i < NB_TILES; i++)
				piece_limit(range.begin(), range.end(), &pieces[i]);
		}
};

class DragonDraw {
	public:
		DragonDraw(const DragonDraw &dragon, TidMap* tidMap) 
		{
			this->drawData = dragon.drawData;
			this->tidMap = tidMap;
		}
		
		DragonDraw(draw_data *drawData, TidMap* tidMap) 
		{ 
			this->drawData = drawData;
			this->tidMap = tidMap;
		}
		
		void operator()(const blocked_range<uint64_t> &range) const
		{
			this->drawData->id = tidMap->getIdFromTid(gettid());

			string msg = "(TBB)Thread no: %d, interval : (%d - %d), gettid() : %d\n";
			const char *array = msg.c_str();
			printf_threadsafe(array, interval++, range.begin(), range.end(), gettid());

			xy_t position;
			xy_t orientation;
			uint64_t j;
			for (size_t i = 0; i < NB_TILES; i++)
			{
				position = compute_position(i, range.begin());
				orientation = compute_orientation(i, range.begin());
				position.x -= this->drawData->limits.minimums.x;
				position.y -= this->drawData->limits.minimums.y;

				for (j = range.begin() + 1; j <= range.end(); j++)
				{
					int m = (position.x + (position.x + orientation.x)) >> 1;
					int n = (position.y + (position.y + orientation.y)) >> 1;
					int index = n * this->drawData->dragon_width + m;

					this->drawData->dragon[index] = j * this->drawData->nb_thread / this->drawData->size;

					position.x += orientation.x;
					position.y += orientation.y;
					if (((j & -j) << 1) & j)
						rotate_left(&orientation);
					else
						rotate_right(&orientation);
				}
			}
		}

  private:
		draw_data *drawData;
		TidMap *tidMap;
};

class DragonRender {
	public:
		DragonRender(const DragonRender &dragon) 
		{
			this->drawData = dragon.drawData; 
		}
		
		DragonRender(draw_data *drawData) 
		{
			this->drawData = drawData;
		}

		void operator()(const blocked_range<uint64_t> &range) const
		{
			scale_dragon(range.begin(), range.end(), this->drawData->image, this->drawData->image_width,this->drawData->image_height,
				this->drawData->dragon, this->drawData->dragon_width, this->drawData->dragon_height,this->drawData->palette);
		}

  private:
		draw_data *drawData;
};

class DragonClear {
	public:
		DragonClear(const DragonClear &dragon)
		{
			this->drawData = dragon.drawData;
		}
		
		DragonClear(draw_data *drawData)
		{
			this->drawData = drawData;
		}

		void operator()(const blocked_range<uint64_t> &range) const
		{
			init_canvas(range.begin(), range.end(), this->drawData->dragon, -1);
		}

  private:
		draw_data *drawData;
};

int dragon_draw_tbb(char **canvas, struct rgb *image, int width, int height, uint64_t size, int nb_thread)
{
	//TODO("dragon_draw_tbb");
	struct draw_data data;
	limits_t limits;
	char *dragon = NULL;
	int dragon_width;
	int dragon_height;
	int dragon_surface;
	int scale_x;
	int scale_y;
	int scale;
	int deltaJ;
	int deltaI;
	struct palette *palette = init_palette(nb_thread);
	if (palette == NULL)
		return -1;

	/* 1. Calculer les limites du dragon */
	dragon_limits_tbb(&limits, size, nb_thread);

	task_scheduler_init init(nb_thread);
	
	dragon_width = limits.maximums.x - limits.minimums.x;
	dragon_height = limits.maximums.y - limits.minimums.y;
	dragon_surface = dragon_width * dragon_height;
	scale_x = dragon_width / width + 1;
	scale_y = dragon_height / height + 1;
	scale = (scale_x > scale_y ? scale_x : scale_y);
	deltaJ = (scale * width - dragon_width) / 2;
	deltaI = (scale * height - dragon_height) / 2;

	dragon = (char *) malloc(dragon_surface);
	if (dragon == NULL) {
		free_palette(palette);
		return -1;
	}

	data.nb_thread = nb_thread;
	data.dragon = dragon;
	data.image = image;
	data.size = size;
	data.image_height = height;
	data.image_width = width;
	data.dragon_width = dragon_width;
	data.dragon_height = dragon_height;
	data.limits = limits;
	data.scale = scale;
	data.deltaI = deltaI;
	data.deltaJ = deltaJ;
	data.palette = palette;
	data.tid = (int *) calloc(nb_thread, sizeof(int));

	/* 2. Initialiser la surface : DragonClear */
	DragonClear dragon_clear(&data);
	parallel_for(blocked_range<uint64_t>(0, dragon_surface), dragon_clear);

	/* 3. Dessiner le dragon : DragonDraw */
	TidMap* tidMap = new TidMap(nb_thread);
	interval = 0;
	DragonDraw dragon_draw(&data, tidMap);
	parallel_for(blocked_range<uint64_t>(0, data.size), dragon_draw);

	/* 4. Effectuer le rendu final */
	DragonRender dragon_render(&data);
	parallel_for(blocked_range<uint64_t>(0, data.image_height), dragon_render);
	
	init.terminate();
	
	free_palette(palette);
	FREE(data.tid);
	tidMap->dump();
	FREE(tidMap);
	*canvas = dragon;
	//*canvas = NULL; // TODO: Retourner le dragon calculé
	return 0;
}

/*
 * Calcule les limites en terme de largeur et de hauteur de
 * la forme du dragon. Requis pour allouer la matrice de dessin.
 */
int dragon_limits_tbb(limits_t *limits, uint64_t size, int nb_thread)
{
	//TODO("dragon_limits_tbb");
	DragonLimits lim;

	/* 1. Calculer les limites */
	task_scheduler_init init(nb_thread);
	parallel_reduce(blocked_range<uint64_t>(0, size), lim);
	
	/* La limite globale est calculée à partir des limites
	 * de chaque dragon.
	 */
	merge_limits(&lim.pieces[0].limits, &lim.pieces[1].limits);
	merge_limits(&lim.pieces[0].limits, &lim.pieces[2].limits);
	merge_limits(&lim.pieces[0].limits, &lim.pieces[3].limits);

	*limits = lim.pieces[0].limits;
	return 0;
}
