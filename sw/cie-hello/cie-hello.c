#include <klib.h>

#ifndef HELLO_USER
#define HELLO_USER "unknown"
#endif
#ifndef HELLO_IP
#define HELLO_IP "0.0.0.0"
#endif

int main()
{
    /* 阶段一要求：输出 hello xiangshan, I am xxx, IP address */
    printf("hello xiangshan, I am %s, %s\n", HELLO_USER, HELLO_IP);
    return 0;
}
