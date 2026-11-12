// 对照：复数类。它不管理任何资源，所以析构、拷贝、移动一个都不用写（零法则）。
#include <cassert>
#include <cmath>
#include <iostream>

class Complex {
public:
    constexpr Complex(double re = 0, double im = 0) : re_(re), im_(im) {}

    constexpr double re() const { return re_; }
    constexpr double im() const { return im_; }
    double abs() const { return std::hypot(re_, im_); }

    friend constexpr Complex operator+(Complex a, Complex b) { return {a.re_ + b.re_, a.im_ + b.im_}; }
    friend constexpr Complex operator*(Complex a, Complex b) {
        return {a.re_ * b.re_ - a.im_ * b.im_, a.re_ * b.im_ + a.im_ * b.re_};
    }
    friend constexpr bool operator==(Complex a, Complex b) { return a.re_ == b.re_ && a.im_ == b.im_; }
    friend std::ostream& operator<<(std::ostream& out, Complex c) {
        return out << c.re_ << (c.im_ < 0 ? "" : "+") << c.im_ << "i";
    }

private:
    double re_;
    double im_;
};

int main() {
    const Complex i(0, 1);
    assert(i * i == Complex(-1, 0));           // i² = -1
    assert(Complex(1, 2) + Complex(3, 4) == Complex(4, 6));
    assert(Complex(3, 4).abs() == 5.0);

    Complex a(1, 2);
    Complex b = a;                              // 编译器自动生成的拷贝就是对的：逐个成员复制
    assert(a == b);
    std::cout << Complex(1, -2) << " 全部通过\n";
    return 0;
}
