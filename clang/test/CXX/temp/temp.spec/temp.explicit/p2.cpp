// RUN: %clang_cc1 -fsyntax-only -verify %s

// Example from the standard
template<class T> class Array { void mf() { } }; 

template class Array<char>; 
template void Array<int>::mf();
template<class T> void sort(Array<T>& v) { /* ... */ }
template void sort(Array<char>&);
namespace N { 
  template<class T> void f(T&) { }
} 
template void N::f<int>(int&);


template<typename T>
struct X0 {
  struct Inner {};
  void f() { }
  static T value;
};

template<typename T>
T X0<T>::value = 17;

typedef X0<int> XInt;

template struct XInt::Inner; // expected-error{{template-id}}
template void XInt::f(); // expected-error{{template-id}}
template int XInt::value; // expected-error{{template-id}}

namespace N {
  template<typename T>
  struct X1 { // expected-note{{explicit instantiation refers here}}
  };
  
  template<typename T>
  void f1(T) {}; // expected-note{{explicit instantiation refers here}}
}
using namespace N;

template struct X1<int>; // expected-error{{must occur in}}
template void f1(int); // expected-error{{must occur in}}
