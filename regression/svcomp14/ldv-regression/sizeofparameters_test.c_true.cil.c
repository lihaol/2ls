/* Generated by CIL v. 1.3.7 */
/* print_CIL_Input is true */

#line 2 "./assert.h"
void __blast_assert(void) 
{ 

  {
  ERROR: 
#line 4
  assert(0);
}
}
#line 5 "files/sizeofparameters_test.c"
void foo(int a ) ;
#line 7 "files/sizeofparameters_test.c"
int globalSize  ;
#line 9 "files/sizeofparameters_test.c"
int main(int argc , char **argv ) 
{ long a ;

  {
  {
#line 12
  globalSize = (int )4U;
#line 13
  foo(a);
  }
#line 14
  return (0);
}
}
#line 17 "files/sizeofparameters_test.c"
void foo(int a ) 
{ unsigned int __cil_tmp2 ;

  {
  {
#line 18
  __cil_tmp2 = (unsigned int )globalSize;
#line 18
  if (4U == __cil_tmp2) {

  } else {
    {
#line 18
    __blast_assert();
    }
  }
  }
#line 19
  return;
}
}
