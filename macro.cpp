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
  printf("%*s => Hello from macro\n", indent, "");
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

