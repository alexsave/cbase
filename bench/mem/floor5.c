#include <pthread.h>
#include <unistd.h>
static void*f(void*a){(void)a;for(;;)pause();return 0;}
int main(void){pthread_t t[5];for(int i=0;i<5;i++)pthread_create(&t[i],0,f,0);for(;;)pause();}
