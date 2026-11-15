#ifndef API_H
#define API_H
#include <ppu-types.h>

struct Place { float lon, lat; char id[16]; char title[36]; char country[28]; u16 size; };
struct Station { char id[16]; char name[56]; char sub[40]; };

#define MAX_STATIONS 400

extern Place *g_places; extern int g_nplaces;
extern Station g_stations[MAX_STATIONS]; extern int g_nstations;

bool api_load_places(char *err, size_t errlen);
bool api_load_stations(const char *place_id, char *err, size_t errlen);
void api_stream_url(const char *channel_id, char *out, size_t outlen);
#endif
