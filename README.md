# llvm-macro
Implementation of "macro" on llvm IR. This is a way to transform every
functions in a C/C++ program during compilation by writing code also in C/C++.
For example, this can be used to insert instrumentation calls on function
entries or exits, but there could be many more possible usage.

## Usage
Building:
```sh
$ make
```
Building with test:
```sh
$ make all
```
The LLVM installation will be located using `llvm-config`. It is important that
the version of LLVM located is the same as that used for the version of `clang`
that will make use of the plugin.

Compiling the macro:
```sh
$ clang++ -o macro.bc macro.cpp -Xclang -disable-O0-optnone -c -emit-llvm
```
It is important that the output format is binary LLVM bitcode (.bc) instead of
textual LLVM bitcode (.ll) when the plugin is used via clang.

Compiling a source file with the macro:
```sh
$ clang++ -o main main.cpp -fplugin=./libmacro.so -fpass-plugin=./libmacro.so -mllvm -macro -mllvm macro.bc
```

## Introduction
### Basic Example
This is the most basic macro written in c++.
```c
#include "macro.h"

#include <stdio.h>

void macro_def()
{
    printf("Entry\n");
    macro_call();
    printf("Exit\n");
}
```

On the first line, we make a include to [macro.h](./macro.h). This simply makes
forward declarations of builtin functions such as `macro_call()` as in the
above example and also ensure that the definition of `macro_def()` have correct
linkage (i.e. extern "C" linkage) without name mangling. It is possible to do
away with the include if you make the correct forward declaration and
definition.

This macro simply insert calls to `printf` before and after every function
calls. The call to `macro_call()` builtin is where the original function is
called.

### C++ destructors
One should be careful about throwing exception in macro due to the lack of c++
destructor information in llvm IR. If an exception is thrown inside a macro
(not in the function wrapped by the macro), the destructors of function
parameters and already returned object after the call to `macro_call()` will be
skipped.

### Multiple calls to `macro_call()`
```c
#include "macro.h"

#include <stdio.h>

void macro_def()
{
    printf("Entry\n");
    macro_call();
    macro_call();
    printf("Exit\n");
}
```

It is possible to have more than one calls to `macro_call()` in a macro. Return
value from later call to `macro_call()` simply overwrite that of previous call.
This behavior is most likely undesired. This also break c++ destructor semantic
since we do not have neccessary information to destruct the previous object at
llvm IR level.

A more likely use case of the feature is as follow:
```c
#include "macro.h"

#include <stdio.h>

void macro_def()
{
    printf("Entry\n");
    if(some_condition())
    {
        do_something();
        macro_call();
        do_something();
    }
    else
    {
        do_otherthing();
        macro_call();
        do_otherthing();
    }
    printf("Exit\n");
}
```
While there are multiple calls to `macro_call()`, it is only called once in
every possible control flows.

### Macro local variables
It is also possible to define macro local variables, which are variables that
are defined once for each functions that a macro is expanded into. For example:

```c
#include "macro.h"

#include <stdio.h>

void macro_def()
{
    printf("this function is called %u times\n", ++macro_local(unsigned));
    macro_call();
}
```

### Builtins
A lot of the functionality are implemented using "builtins" which are special
functions with extern linkage as declared in [macro.h](./macro.h) prefixed with
`macro_`. They are replaced with their actual "implementations" using a llvm
module pass when the macro is applied. Currently, there are 3 builtins:

 - `size_t macro_count(void)`
    - return the number of times the macro is expanded into a function

 - `size_t macro_index(void)`
    - return a sequentially-allocated index starting from 0 each times the
      macro is expaneded into a function

 - `void *macro_array(size_t id, size_t size, size_t alignment)`
    - return a statically allocated array of given size and alignment keyed by
      id

