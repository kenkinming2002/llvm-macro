#ifndef MACRO_H
#define MACRO_H

#include <stddef.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

void macro_def(void);
void macro_call(void);

size_t macro_count(void);
size_t macro_index(void);
void *macro_array(size_t id, size_t size, size_t alignment);

#define macro_local(type) (((type *)macro_array(__LINE__, sizeof(type), alignof(type)))[macro_index()])

#ifdef __cplusplus
}
#endif

#endif // MACRO_H
