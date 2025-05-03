#include "macro.h"

#include <stdio.h>
#include <stdlib.h>

static int indent;

static int roulette(int a)
{
  return a % 3;
}

static void say_hello(void)
{
  static int next_id;
  printf("%*s => Hello from macro => id %d, called %d times\n", indent, "", next_id, ++macro_local(int));
}

void macro_def(void)
{
  printf("%*s => Begin\n", indent, "");

  say_hello();

  switch(roulette(rand()))
  {
  case 0:
    indent += 4;
    macro_call();
    indent -= 4;

    printf("%*s => Lucky End\n", indent, "");
    return;
  case 1:
    indent += 2;
    macro_call();
    indent -= 2;

    printf("%*s => Unlucky End\n", indent, "");
    return;
  }

  indent += 3;
  macro_call();
  indent -= 3;

  printf("%*s => Mundane End\n", indent, "");
}

