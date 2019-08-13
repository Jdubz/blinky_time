
int gcd(int a, int b) {  
  if (a == 0) 
    return b;  
  return gcd(b % a, a);  
}  
  
// Function to return LCM of two numbers  
int lcm(int a, int b) {  
  return (a*b)/gcd(a, b);  
}  
  
