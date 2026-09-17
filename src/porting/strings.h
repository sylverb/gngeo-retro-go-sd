#ifndef STRINGS_H
#define STRINGS_H
#include <string.h>
#ifndef bzero
#define bzero(p, n) memset((p), 0, (n))
#endif
#ifndef bcopy
#define bcopy(s, d, n) memmove((d), (s), (n))
#endif
#endif
