#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void say_hello(void)
{
  printf("Hello from test\n");
}

void foo()
{
  say_hello();
  printf("Me is foo\n");
}

int bar(int n)
{
  int a = 1, b = 1;
  for(int i=0; i<n; ++i)
  {
    int c = a + b;
    a = b;
    b = c;
  }
  return a;
}

void baz(int n, ...)
{
  va_list ap;
  va_start(ap, n);
  for(int i=0; i<n; ++i)
    printf("got %d\n", va_arg(ap, int));
  va_end(ap);
}

int main()
{
  baz(3, bar(1), bar(2), bar(4));
  srand(time(NULL));
  foo();
  return bar(10);
}
