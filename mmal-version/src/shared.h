#ifndef SHARED_H
#define SHARED_H

extern pthread_mutex_t running_mutex;
extern bool running;

extern pthread_mutex_t restart_mutex;
extern int restart;

#endif