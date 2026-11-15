#ifndef PLAYER_H
#define PLAYER_H
enum PState { P_IDLE, P_CONNECTING, P_BUFFERING, P_PLAYING, P_ERROR };

bool player_init();            /* opens audio port, starts output thread */
void player_shutdown();
void player_play(const char *url);   /* stops current stream first */
void player_stop();
PState player_state();
const char *player_message();
void player_set_volume(float v);     /* 0..1 */
float player_get_volume();
#endif
