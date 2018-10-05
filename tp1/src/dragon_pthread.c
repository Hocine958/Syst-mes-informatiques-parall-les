/*
 * dragon_pthread.c
 *
 *  Created on: 2011-08-17
 *      Author: Francis Giraldeau <francis.giraldeau@gmail.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "utils.h"
#include "dragon.h"
#include "color.h"
#include "dragon_pthread.h"

#define PRINT_PTHREAD_ERROR(err, msg) \
	do { errno = err; perror(msg); } while(0)

pthread_mutex_t mutex_stdout;

void printf_threadsafe(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	pthread_mutex_lock(&mutex_stdout);
	vprintf(format, ap);
	pthread_mutex_unlock(&mutex_stdout);
	va_end(ap);
}

/**
 * Does the work on a part of the dragon.
 * Returns 0 on success.
 */
void* dragon_draw_worker(void *data)
{
	struct draw_data *drawData = (struct draw_data *) data;
	
	/* 1. Initialiser la surface */
	int area = drawData->dragon_width * drawData->dragon_height;
	
	int step = area / drawData->nb_thread;
	int start = drawData->id * step;
	int end;

	if (drawData->id == (drawData->nb_thread - 1))
		end = area;
	else
		end = step * (drawData->id + 1);
	
	init_canvas(start, end, drawData->dragon, -1);
	
	pthread_barrier_wait(drawData->barrier);

	/* 2. Dessiner les dragons dans les 4 directions
	 *
	 * Il est attendu que chaque threads dessine une partie
	 * de chaque dragon.
	 * */

	start = drawData->id * drawData->size / drawData->nb_thread;
	end = (drawData->id + 1) * drawData->size / drawData->nb_thread;
	
	printf_threadsafe("Thread no: %d, interval: (%d - %d), gettid(): %d \n", drawData->id, start, end, gettid());
	
	for(int i = 0; i < 4; i++){
		dragon_draw_raw(i, start, end, drawData->dragon, drawData->dragon_width, drawData->dragon_height, drawData->limits, drawData->id);
	}
	
	pthread_barrier_wait(drawData->barrier);
	
	/* 3. Effectuer le rendu final */
	start = drawData->id * drawData->image_height / drawData->nb_thread;
	end = (drawData->id + 1) * drawData->image_height / drawData->nb_thread;
	
	scale_dragon(start, end, drawData->image, drawData->image_width, drawData->image_height, drawData->dragon, drawData->dragon_width, drawData->dragon_height, drawData->palette);

	return NULL;
}

int dragon_draw_pthread(char **canvas, struct rgb *image, int width, int height, uint64_t size, int nb_thread)
{
	//TODO("dragon_draw_pthread");

	pthread_t *threads = NULL;
	pthread_barrier_t barrier;
	limits_t lim;
	struct draw_data info;
	char *dragon = NULL;
	int scale_x;
	int scale_y;
	struct draw_data *data = NULL;
	struct palette *palette = NULL;
	int ret = 0;

	palette = init_palette(nb_thread);
	if (palette == NULL)
		goto err;

	/* 1. Initialiser barrier. */
	int s = pthread_barrier_init(&barrier, NULL, nb_thread);
    if (s != 0){
		printf("draw pthread_barrier_init error : " + s + '\n');
		goto err;
	}

	if (dragon_limits_pthread(&lim, size, nb_thread) < 0)
		goto err;

	info.dragon_width = lim.maximums.x - lim.minimums.x;
	info.dragon_height = lim.maximums.y - lim.minimums.y;

	if ((dragon = (char *) malloc(info.dragon_width * info.dragon_height)) == NULL) {
		printf("malloc error dragon\n");
		goto err;
	}

	if ((data = malloc(sizeof(struct draw_data) * nb_thread)) == NULL) {
		printf("malloc error data\n");
		goto err;
	}

	if ((threads = malloc(sizeof(pthread_t) * nb_thread)) == NULL) {
		printf("malloc error threads\n");
		goto err;
	}

	info.image_height = height;
	info.image_width = width;
	scale_x = info.dragon_width / width + 1;
	scale_y = info.dragon_height / height + 1;
	info.scale = (scale_x > scale_y ? scale_x : scale_y);
	info.deltaJ = (info.scale * width - info.dragon_width) / 2;
	info.deltaI = (info.scale * height - info.dragon_height) / 2;
	info.nb_thread = nb_thread;
	info.dragon = dragon;
	info.image = image;
	info.size = size;
	info.limits = lim;
	info.barrier = &barrier;
	info.palette = palette;
    
	/* 2. Lancement du calcul parallèle principal avec dragon_draw_worker */
	for (int i = 0; i < nb_thread; i++) {
		memcpy(&data[i], &info, sizeof(struct draw_data));
		//data[i] = info;
		data[i].id = i;
		if(pthread_create(&threads[i], NULL, dragon_draw_worker, &data[i]) != 0) {
			printf("draw pthread_create error\n");
			goto err;	
		}
	}
	
	/* 3. Attendre la fin du traitement */
	void *res;
	for (int i = 0; i < nb_thread; i++) {
		int retVal = pthread_join(threads[i], &res);
		if(retVal != 0){
			printf("draw pthread_join error : " + retVal + '\n');
			goto err;
		}
	}
	
	/* 4. Destruction des variables. */
	int des = pthread_barrier_destroy(&barrier);
	if(des != 0){
		printf("draw pthread_barrier_destroy error : " + des + '\n');
		goto err;
	}
	
done:
	FREE(data);
	FREE(threads);
	free_palette(palette);
	*canvas = dragon;
	//*canvas = NULL; // TODO: retourner le dragon calculé
	return ret;

err:
	FREE(dragon);
	ret = -1;
	goto done;
}

void *dragon_limit_worker(void *data)
{
	int i;
	struct limit_data *lim = (struct limit_data *) data;
	int start = lim->start;
	int end = lim->end;

	for (i = 0; i < NB_TILES; i++) {
		piece_limit(start, end, &lim->pieces[i]);
	}

	return NULL;
}

/*
 * Calcule les limites en terme de largeur et de hauteur de
 * la forme du dragon. Requis pour allouer la matrice de dessin.
 */
int dragon_limits_pthread(limits_t *limits, uint64_t size, int nb_thread)
{
	//TODO("dragon_limits_pthread");

	int ret = 0;
	int i;
	pthread_t *threads = NULL;
	struct limit_data *thread_data = NULL;
	piece_t masters[NB_TILES];

	for (i = 0; i < NB_TILES; i++) {
		/**
		 * La pièce master représente les limites d'un dragon complet.
		 * Notez bien que chaque dragon à une orientation différente.
		 */
		piece_init(&masters[i]);
		masters[i].orientation = tiles_orientation[i];
	}
	
	/* 1. Allouer de l'espace pour threads et threads_data. */
	if((threads = malloc(sizeof(pthread_t) * nb_thread)) == NULL) {
		printf("limits threads malloc error\n");
		goto err;	
	}
	
	if((thread_data = malloc(sizeof(struct limit_data) * nb_thread)) == NULL) {
		printf("limits thread_data malloc error\n");
		goto err;	
	}

	/* 2. Lancement du calcul en parallèle avec dragon_limit_worker. */	
	for (int i = 0; i < nb_thread; i++) {
		thread_data[i].id = i;
		thread_data[i].start = i * size / nb_thread;
		thread_data[i].end = (i + 1) * size / nb_thread;
		
		for (int j = 0; j < NB_TILES; j++)
			thread_data[i].pieces[j] = masters[j];
			
		if(pthread_create(&threads[i], NULL, dragon_limit_worker, &thread_data[i]) != 0) {
			printf("limits pthread_create error\n");
			goto err;	
		}
	}
	
	/* 3. Attendre la fin du traitement. */
	void *res;
	for (int i = 0; i < nb_thread; i++) {
		int retVal = pthread_join(threads[i], &res);
		if(retVal != 0){
			printf("limits pthread_join error : " + retVal + '\n');
			goto err;
		}
	}
	
	/* 4. Fusion des pièces.
	 *
	 * La fonction piece_merge est disponible afin d'accomplir ceci.
	 * Notez bien que les pièces ayant la même orientation initiale
	 * doivent être fusionnées ensemble.
	 * */
	 for (int i = 0; i < NB_TILES; i++) {
		 for (int j = 1; j < nb_thread; j++) {
			piece_merge(&thread_data[0].pieces[i],thread_data[j].pieces[i], tiles_orientation[i]);
		}
		masters[i] = thread_data[0].pieces[i];
	}

	/* La limite globale est calculée à partir des limites
	 * de chaque dragon calculées à l'étape 4.
	 */
	merge_limits(&masters[0].limits, &masters[1].limits);
	merge_limits(&masters[0].limits, &masters[2].limits);
	merge_limits(&masters[0].limits, &masters[3].limits);

done:
	FREE(threads);
	FREE(thread_data);
	*limits = masters[0].limits;
	return ret;
err:
	ret = -1;
	goto done;
}
