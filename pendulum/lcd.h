#ifndef LeastCommonMultiple_h
#define LeastCommonMultiple_h

unsigned long gcd(int a, int b) {  
  if (a == 0) 
    return b;  
  return gcd(b % a, a);  
}  
  
// Function to return LCM of two numbers  
unsigned long lcm(unsigned long a, unsigned long b) {  
  return (a*b)/gcd(a, b);  
}

#endif
