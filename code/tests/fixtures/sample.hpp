// Fixture for ast_analyzer tests — small but covers all the AST node
// kinds we care about: includes, classes with inheritance, free function,
// static method, virtual method, struct.
#include <string>
#include <vector>

namespace demo {

struct Point {
    int x{0};
    int y{0};
};

class Shape {
public:
    Shape();
    virtual ~Shape();
    virtual double area() const = 0;
    static int total_count();

protected:
    int id_{0};
};

class Circle : public Shape {
public:
    explicit Circle(double r);
    double area() const override;
    void set_center(const Point& p);

private:
    double radius_{0.0};
    Point  center_{};
};

int add(int a, int b);

std::string greet(const std::string& name, int times);

}  // namespace demo
