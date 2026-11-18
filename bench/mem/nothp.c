#include <sys/prctl.h>
#include <unistd.h>
int main(int c,char**v){prctl(PR_SET_THP_DISABLE,1,0,0,0);execv(v[1],v+1);return 127;}
