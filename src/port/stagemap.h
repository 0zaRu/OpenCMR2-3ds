/*
 * OpenCMR2 stage map, for a handheld's second screen (the Nintendo 3DS's
 * bottom screen, src/platform/ctr_map.cpp); nothing elsewhere.
 */
#ifndef OPENCMR2_PORT_STAGEMAP_H
#define OPENCMR2_PORT_STAGEMAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once a frame while a stage runs. The route: routeCount nodes, stride bytes
   apart, x at position[0] and z at position[2] (16.16 fixed point), split
   points where bit 0 of the node's flags byte is set (NULL: no route). The
   cars: x, z pairs (16.16), the player's first, and the player's heading as
   the x and z of its forward axis. */
void StageMap_Update(const int *position, const unsigned char *flags, int routeCount, int stride,
                     const int *cars, int carCount, int forwardX, int forwardZ);

#ifdef __cplusplus
}
#endif

#endif
